#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* PNG decoder that shrinks the picture while it decodes, so a 400 x 300
 * thumbnail never needs more than two source rows in memory. Pictures up to
 * 1024 pixels on a side; 8-bit grey, grey+alpha, RGB, RGBA and palette
 * images, not interlaced. Transparent pixels are blended onto a background
 * colour. Memory: one 32 KB block for the inflate window, the rest small. */

#define PNG_MAX_STRIPS 8

/* A decoded picture as RGB565, row after row, cut into horizontal strips of
 * strip_rows rows (the last may have fewer) so no piece needs a large block
 * of memory. */
typedef struct {
    int w, h;
    int strip_rows;
    int strips;
    uint16_t *px[PNG_MAX_STRIPS];
} png_picture_t;

/* A part of the picture in source pixels; x1 and y1 are exclusive. */
typedef struct {
    int x0, y0, x1, y1;
} png_box_t;

/* Fills buf with up to len bytes of the file: returns the count, 0 at the
 * end, < 0 on error. */
typedef int (*png_read_fn)(void *ctx, uint8_t *buf, size_t len);

/* Decodes the file read() delivers, or the part of it in box (NULL: all of
 * it; a box outside the picture is cut to it), scaled down (never up) to fit
 * max_w x max_h with its shape kept. bg is 0xRRGGBB. Returns NULL with a
 * reason in *error when the file is broken or unsupported or memory runs
 * out. Free the result with png_picture_free. */
png_picture_t *png_decode(png_read_fn read, void *ctx, int max_w, int max_h, uint32_t bg,
                          const png_box_t *box, const char **error);

/* Finds the smallest box holding every pixel that is not fully transparent.
 * A picture without transparency, or with nothing visible, gives the whole
 * picture; one without an alpha channel is not inflated at all. Same memory
 * as png_decode, without the result. False with a reason in *error when the
 * file is broken or unsupported. */
bool png_alpha_box(png_read_fn read, void *ctx, png_box_t *out, const char **error);

void png_picture_free(png_picture_t *pic);
