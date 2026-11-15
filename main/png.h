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

/* Fills buf with up to len bytes of the file: returns the count, 0 at the
 * end, < 0 on error. */
typedef int (*png_read_fn)(void *ctx, uint8_t *buf, size_t len);

/* Decodes the file read() delivers, scaled down (never up) to fit
 * max_w x max_h with its shape kept. bg is 0xRRGGBB. Returns NULL with a
 * reason in *error when the file is broken or unsupported or memory runs
 * out. Free the result with png_picture_free. */
png_picture_t *png_decode(png_read_fn read, void *ctx, int max_w, int max_h, uint32_t bg,
                          const char **error);

void png_picture_free(png_picture_t *pic);
