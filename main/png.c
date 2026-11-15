#include "png.h"

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

/* A pull decoder: inflate asks for bytes, the chunk layer reads them from
 * the caller as needed. Inflate keeps only canonical Huffman tables (about
 * 1 KB) next to the 32 KB window deflate requires. */

#define MAX_SIDE    1024
#define WINDOW      32768
#define STRIP_BYTES 4096 /* result pieces small enough for a split heap */
#define IN_BUF      512

enum { C_GREY = 0, C_RGB = 2, C_PALETTE = 3, C_GREY_ALPHA = 4, C_RGBA = 6 };

#define TYPE(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (d))

typedef struct {
    int16_t count[16]; /* codes of each length */
    int16_t symbol[288];
} huff_t;

typedef struct {
    png_read_fn read;
    void *ctx;
    jmp_buf fail;
    const char *error;

    /* input */
    uint8_t in[IN_BUF];
    int in_pos, in_len;
    uint32_t chunk_left; /* IDAT bytes left in the current chunk */
    uint32_t bitbuf;
    int bitcnt;

    /* image */
    int max_w, max_h;
    uint8_t bg[3];
    uint32_t w, h;
    uint8_t ctype;
    int bpp;
    uint32_t line_len; /* filter byte + w * bpp */
    uint8_t pal[256 * 3];
    uint8_t alpha[256];

    /* inflate */
    uint8_t *window;
    uint32_t wpos;
    huff_t lencode, distcode;

    /* rows */
    uint8_t *cur, *prev;
    uint32_t line_pos;
    uint32_t y;

    /* output */
    int ow, oh;
    int acc_row;
    uint32_t *acc; /* r, g, b, count per output column */
    png_picture_t *pic;
    bool done;
} dec_t;

static void __attribute__((noreturn)) fail(dec_t *d, const char *why)
{
    d->error = why;
    longjmp(d->fail, 1);
}

static uint32_t be32(const uint8_t *b)
{
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

void png_picture_free(png_picture_t *pic)
{
    if (pic) {
        for (int i = 0; i < pic->strips; i++) {
            free(pic->px[i]);
        }
        free(pic);
    }
}

/* ---- input ------------------------------------------------------------- */

static uint8_t raw_byte(dec_t *d)
{
    if (d->in_pos == d->in_len) {
        int n = d->read(d->ctx, d->in, sizeof(d->in));
        if (n < 0) {
            fail(d, "read failed");
        }
        if (n == 0) {
            fail(d, "incomplete image");
        }
        d->in_pos = 0;
        d->in_len = n;
    }
    return d->in[d->in_pos++];
}

static void raw_read(dec_t *d, uint8_t *buf, uint32_t n)
{
    while (n--) {
        *buf++ = raw_byte(d);
    }
}

static void raw_skip(dec_t *d, uint32_t n)
{
    while (n--) {
        raw_byte(d);
    }
}

/* Reads a chunk header; returns its type, length in *len. */
static uint32_t chunk_header(dec_t *d, uint32_t *len)
{
    uint8_t b[8];
    raw_read(d, b, 8);
    *len = be32(b);
    if (*len > 0x7fffffff) {
        fail(d, "not a PNG file");
    }
    return be32(b + 4);
}

/* Handles a chunk other than IDAT. */
static void other_chunk(dec_t *d, uint32_t type, uint32_t len)
{
    if (type == TYPE('P', 'L', 'T', 'E') && len <= sizeof(d->pal)) {
        raw_read(d, d->pal, len);
    } else if (type == TYPE('t', 'R', 'N', 'S') && d->ctype == C_PALETTE &&
               len <= sizeof(d->alpha)) {
        raw_read(d, d->alpha, len);
    } else if (type == TYPE('I', 'E', 'N', 'D')) {
        fail(d, "incomplete image");
    } else {
        raw_skip(d, len);
    }
    raw_skip(d, 4); /* CRC: TCP already checked the bytes */
}

/* The next byte of the compressed stream, across IDAT chunks. */
static uint8_t idat_byte(dec_t *d)
{
    while (d->chunk_left == 0) {
        raw_skip(d, 4); /* CRC of the IDAT just finished */
        uint32_t len, type;
        while ((type = chunk_header(d, &len)) != TYPE('I', 'D', 'A', 'T')) {
            other_chunk(d, type, len);
        }
        d->chunk_left = len; /* an empty IDAT goes round again */
    }
    d->chunk_left--;
    return raw_byte(d);
}

/* ---- rows -------------------------------------------------------------- */

static int paeth(int a, int b, int c)
{
    int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static void unfilter(dec_t *d)
{
    uint8_t *c = d->cur + 1;
    const uint8_t *u = d->prev + 1;
    uint32_t n = d->line_len - 1, bpp = d->bpp;

    switch (d->cur[0]) {
    case 0:
        break;
    case 1:
        for (uint32_t i = bpp; i < n; i++) {
            c[i] += c[i - bpp];
        }
        break;
    case 2:
        for (uint32_t i = 0; i < n; i++) {
            c[i] += u[i];
        }
        break;
    case 3:
        for (uint32_t i = 0; i < n; i++) {
            c[i] += ((i >= bpp ? c[i - bpp] : 0) + u[i]) >> 1;
        }
        break;
    case 4:
        for (uint32_t i = 0; i < n; i++) {
            c[i] += paeth(i >= bpp ? c[i - bpp] : 0, u[i], i >= bpp ? u[i - bpp] : 0);
        }
        break;
    default:
        fail(d, "corrupt image data");
    }
}

static void flush_row(dec_t *d)
{
    if (d->acc_row < 0) {
        return;
    }
    const png_picture_t *pic = d->pic;
    uint16_t *dst = pic->px[d->acc_row / pic->strip_rows] +
                    (size_t)(d->acc_row % pic->strip_rows) * d->ow;
    for (int x = 0; x < d->ow; x++) {
        uint32_t *a = d->acc + x * 4;
        uint32_t n = a[3] ? a[3] : 1;
        uint32_t r = a[0] / n, g = a[1] / n, b = a[2] / n;
        dst[x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
    }
    memset(d->acc, 0, (size_t)d->ow * 4 * sizeof(uint32_t));
}

/* Box filter: every source pixel adds to the output pixel it falls in.
 * Transparent pixels are blended onto the background first. */
static void take_row(dec_t *d)
{
    int oy = (int)((uint64_t)d->y * d->oh / d->h);
    if (oy != d->acc_row) {
        flush_row(d);
        d->acc_row = oy;
    }
    const uint8_t *s = d->cur + 1;
    for (uint32_t x = 0; x < d->w; x++, s += d->bpp) {
        uint32_t c[3], a = 255;
        switch (d->ctype) {
        case C_GREY: c[0] = c[1] = c[2] = s[0]; break;
        case C_GREY_ALPHA: c[0] = c[1] = c[2] = s[0]; a = s[1]; break;
        case C_RGB: c[0] = s[0]; c[1] = s[1]; c[2] = s[2]; break;
        case C_RGBA: c[0] = s[0]; c[1] = s[1]; c[2] = s[2]; a = s[3]; break;
        default: /* palette */
            c[0] = d->pal[s[0] * 3];
            c[1] = d->pal[s[0] * 3 + 1];
            c[2] = d->pal[s[0] * 3 + 2];
            a = d->alpha[s[0]];
            break;
        }
        uint32_t *acc = d->acc + (uint32_t)((uint64_t)x * d->ow / d->w) * 4;
        for (int k = 0; k < 3; k++) {
            if (a != 255) {
                c[k] = (c[k] * a + d->bg[k] * (255 - a) + 127) / 255;
            }
            acc[k] += c[k];
        }
        acc[3]++;
    }
}

/* One byte of inflated data: into the window, and into the current row. */
static void put(dec_t *d, uint8_t b)
{
    d->window[d->wpos++ & (WINDOW - 1)] = b;
    if (d->y >= d->h) {
        return; /* trailing data after the last row */
    }
    d->cur[d->line_pos++] = b;
    if (d->line_pos == d->line_len) {
        unfilter(d);
        take_row(d);
        uint8_t *t = d->prev;
        d->prev = d->cur;
        d->cur = t;
        d->line_pos = 0;
        d->y++;
    }
}

/* ---- inflate (RFC 1951) ------------------------------------------------ */

static uint32_t bits(dec_t *d, int need)
{
    while (d->bitcnt < need) {
        d->bitbuf |= (uint32_t)idat_byte(d) << d->bitcnt;
        d->bitcnt += 8;
    }
    uint32_t v = d->bitbuf & ((1u << need) - 1);
    d->bitbuf >>= need;
    d->bitcnt -= need;
    return v;
}

static void stored(dec_t *d)
{
    d->bitbuf = 0;
    d->bitcnt = 0;
    uint32_t len = idat_byte(d);
    len |= (uint32_t)idat_byte(d) << 8;
    uint32_t nlen = idat_byte(d);
    nlen |= (uint32_t)idat_byte(d) << 8;
    if (len != (~nlen & 0xffff)) {
        fail(d, "corrupt image data");
    }
    while (len--) {
        put(d, idat_byte(d));
    }
}

static int decode(dec_t *d, const huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bits(d, 1);
        int count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    fail(d, "corrupt image data");
}

/* Builds a table from code lengths; false when the lengths are not a valid
 * code. An incomplete code is allowed (single distance codes are common). */
static bool construct(huff_t *h, const int16_t *length, int n)
{
    int16_t offs[16];
    memset(h->count, 0, sizeof(h->count));
    for (int s = 0; s < n; s++) {
        h->count[length[s]]++;
    }
    if (h->count[0] == n) {
        return true;
    }
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) {
            return false;
        }
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) {
        offs[len + 1] = offs[len] + h->count[len];
    }
    for (int s = 0; s < n; s++) {
        if (length[s] != 0) {
            h->symbol[offs[length[s]]++] = s;
        }
    }
    return true;
}

static void codes(dec_t *d)
{
    static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                     2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                       6145, 8193, 12289, 16385, 24577};
    static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                     6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int sym = decode(d, &d->lencode);
        if (sym < 256) {
            put(d, (uint8_t)sym);
            continue;
        }
        if (sym == 256) {
            return;
        }
        sym -= 257;
        if (sym >= 29) {
            fail(d, "corrupt image data");
        }
        uint32_t len = lbase[sym] + bits(d, lext[sym]);
        int ds = decode(d, &d->distcode);
        if (ds >= 30) {
            fail(d, "corrupt image data");
        }
        uint32_t dist = dbase[ds] + bits(d, dext[ds]);
        if (dist > d->wpos) {
            fail(d, "corrupt image data");
        }
        while (len--) {
            put(d, d->window[(d->wpos - dist) & (WINDOW - 1)]);
        }
    }
}

static void fixed(dec_t *d)
{
    int16_t lengths[288];
    int s = 0;
    for (; s < 144; s++) lengths[s] = 8;
    for (; s < 256; s++) lengths[s] = 9;
    for (; s < 280; s++) lengths[s] = 7;
    for (; s < 288; s++) lengths[s] = 8;
    construct(&d->lencode, lengths, 288);
    for (s = 0; s < 30; s++) lengths[s] = 5;
    construct(&d->distcode, lengths, 30);
    codes(d);
}

static void dynamic(dec_t *d)
{
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                      11, 4, 12, 3, 13, 2, 14, 1, 15};
    int16_t lengths[320];
    int nlen = bits(d, 5) + 257, ndist = bits(d, 5) + 1, ncode = bits(d, 4) + 4;
    if (nlen > 286 || ndist > 30) {
        fail(d, "corrupt image data");
    }
    int idx = 0;
    for (; idx < ncode; idx++) lengths[order[idx]] = bits(d, 3);
    for (; idx < 19; idx++) lengths[order[idx]] = 0;
    if (!construct(&d->lencode, lengths, 19)) {
        fail(d, "corrupt image data");
    }
    idx = 0;
    while (idx < nlen + ndist) {
        int sym = decode(d, &d->lencode);
        if (sym < 16) {
            lengths[idx++] = sym;
            continue;
        }
        int len = 0, rep;
        if (sym == 16) {
            if (idx == 0) {
                fail(d, "corrupt image data");
            }
            len = lengths[idx - 1];
            rep = 3 + bits(d, 2);
        } else if (sym == 17) {
            rep = 3 + bits(d, 3);
        } else {
            rep = 11 + bits(d, 7);
        }
        if (idx + rep > nlen + ndist) {
            fail(d, "corrupt image data");
        }
        while (rep--) {
            lengths[idx++] = len;
        }
    }
    if (lengths[256] == 0 || !construct(&d->lencode, lengths, nlen) ||
        !construct(&d->distcode, lengths + nlen, ndist)) {
        fail(d, "corrupt image data");
    }
    codes(d);
}

static void inflate_all(dec_t *d)
{
    uint8_t cmf = idat_byte(d), flg = idat_byte(d);
    if ((cmf & 0x0f) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) {
        fail(d, "corrupt image data");
    }
    int last;
    do {
        last = bits(d, 1);
        switch (bits(d, 2)) {
        case 0: stored(d); break;
        case 1: fixed(d); break;
        case 2: dynamic(d); break;
        default: fail(d, "corrupt image data");
        }
    } while (!last && d->y < d->h);
    /* the Adler-32 and the chunks after are not needed */
}

/* ---- header ------------------------------------------------------------ */

static void read_header(dec_t *d)
{
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    uint8_t b[13];
    raw_read(d, b, 8);
    if (memcmp(b, SIG, 8) != 0) {
        fail(d, "not a PNG file");
    }
    uint32_t len, type = chunk_header(d, &len);
    if (type != TYPE('I', 'H', 'D', 'R') || len != 13) {
        fail(d, "not a PNG file");
    }
    raw_read(d, b, 13);
    raw_skip(d, 4);
    d->w = be32(b);
    d->h = be32(b + 4);
    d->ctype = b[9];
    if (d->w < 1 || d->h < 1 || d->w > MAX_SIDE || d->h > MAX_SIDE) {
        fail(d, "image too large");
    }
    if (b[8] != 8 || b[10] != 0 || b[11] != 0) {
        fail(d, "only 8-bit images are supported");
    }
    if (b[12] != 0) {
        fail(d, "interlaced images are not supported");
    }
    switch (d->ctype) {
    case C_GREY: case C_PALETTE: d->bpp = 1; break;
    case C_GREY_ALPHA: d->bpp = 2; break;
    case C_RGB: d->bpp = 3; break;
    case C_RGBA: d->bpp = 4; break;
    default: fail(d, "unknown colour type");
    }
    d->line_len = 1 + d->w * d->bpp;

    /* chunks up to the first IDAT: palette and transparency */
    for (;;) {
        type = chunk_header(d, &len);
        if (type == TYPE('I', 'D', 'A', 'T')) {
            d->chunk_left = len;
            break;
        }
        other_chunk(d, type, len);
    }
}

static void setup_output(dec_t *d)
{
    /* fit inside max_w x max_h, keep the shape, never enlarge */
    d->ow = d->w;
    d->oh = d->h;
    if (d->ow > d->max_w || d->oh > d->max_h) {
        if ((uint64_t)d->w * d->max_h > (uint64_t)d->h * d->max_w) {
            d->ow = d->max_w;
            d->oh = (int)((uint64_t)d->h * d->max_w / d->w);
        } else {
            d->oh = d->max_h;
            d->ow = (int)((uint64_t)d->w * d->max_h / d->h);
        }
        d->ow = d->ow < 1 ? 1 : d->ow;
        d->oh = d->oh < 1 ? 1 : d->oh;
    }

    d->cur = malloc(d->line_len);
    d->prev = calloc(1, d->line_len);
    d->acc = calloc((size_t)d->ow * 4, sizeof(uint32_t));
    d->pic = calloc(1, sizeof(*d->pic));
    if (!d->cur || !d->prev || !d->acc || !d->pic) {
        fail(d, "out of memory");
    }
    png_picture_t *pic = d->pic;
    pic->w = d->ow;
    pic->h = d->oh;
    pic->strip_rows = STRIP_BYTES / (d->ow * 2);
    pic->strip_rows = pic->strip_rows < 1 ? 1 : pic->strip_rows;
    if ((d->oh + pic->strip_rows - 1) / pic->strip_rows > PNG_MAX_STRIPS) {
        pic->strip_rows = (d->oh + PNG_MAX_STRIPS - 1) / PNG_MAX_STRIPS;
    }
    int strips = (d->oh + pic->strip_rows - 1) / pic->strip_rows;
    for (int i = 0; i < strips; i++) {
        int rows = d->oh - i * pic->strip_rows;
        rows = rows < pic->strip_rows ? rows : pic->strip_rows;
        pic->px[i] = malloc((size_t)d->ow * rows * sizeof(uint16_t));
        if (!pic->px[i]) {
            fail(d, "out of memory");
        }
        pic->strips++;
    }
}

/* ---- entry ------------------------------------------------------------- */

png_picture_t *png_decode(png_read_fn read, void *ctx, int max_w, int max_h, uint32_t bg,
                          const char **error)
{
    dec_t *d = calloc(1, sizeof(*d));
    uint8_t *window = malloc(WINDOW);
    if (!d || !window || max_w < 1 || max_h < 1) {
        free(d);
        free(window);
        *error = "out of memory";
        return NULL;
    }
    d->read = read;
    d->ctx = ctx;
    d->window = window;
    d->max_w = max_w;
    d->max_h = max_h;
    d->bg[0] = bg >> 16;
    d->bg[1] = bg >> 8;
    d->bg[2] = bg;
    memset(d->alpha, 0xff, sizeof(d->alpha));
    d->acc_row = -1;

    bool ok = false;
    if (setjmp(d->fail) == 0) {
        read_header(d);
        setup_output(d);
        inflate_all(d);
        if (d->y < d->h) {
            fail(d, "incomplete image");
        }
        flush_row(d);
        d->done = true; /* in the struct: locals may not survive longjmp */
    } else {
        *error = d->error;
    }
    ok = d->done;
    png_picture_t *pic = ok ? d->pic : NULL;
    if (!ok) {
        png_picture_free(d->pic);
    }
    free(d->cur);
    free(d->prev);
    free(d->acc);
    free(d->window);
    free(d);
    return pic;
}
