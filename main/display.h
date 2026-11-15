#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

/* Brings up the display driver chosen in Kconfig, then LVGL on it. */
esp_err_t display_init(lv_display_t **disp_out);

/* Screen size in pixels, as LVGL sees it. */
int display_width(void);
int display_height(void);

/* Blocks until every queued pixel transfer has reached the panel. */
void display_wait_idle(void);

/* With the LVGL lock held: stop or restart drawing. While frozen the panel
 * keeps showing the last image; unfreezing redraws the whole screen. */
void display_freeze(bool freeze);

/* Small busy mark in the top-right corner, sent straight to the panel. */
void display_draw_mark(void);
