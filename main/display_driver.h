#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/* A display driver brings up its bus and panel and hands back esp_lcd
 * handles. Pixels go out through esp_lcd_panel_draw_bitmap on the panel
 * handle (esp_lvgl_port's flush and the busy mark), so any panel esp_lcd
 * supports fits behind this. */

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    int width, height;              /* as LVGL sees it */
    bool mirror_x, mirror_y, swap_xy;
    bool swap_bytes;                /* RGB565 sent high byte first */
} display_panel_t;

typedef struct {
    const char *name;
    /* buf_lines: rows per flush, so the bus can take a whole draw buffer.
     * Leaves the backlight off. */
    esp_err_t (*init)(int buf_lines, display_panel_t *out);
    void (*backlight)(bool on);
} display_driver_t;

extern const display_driver_t display_st7796;
