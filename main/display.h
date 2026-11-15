#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "lvgl.h"

typedef struct {
    uint8_t id4[4];       /* raw 0xD3 response, starts with one dummy bit */
    uint8_t rddid[4];     /* raw 0x04 response, starts with one dummy bit */
    uint32_t id4_value;   /* 24-bit ID4 with the dummy bit removed: 0x007796 */
    uint32_t rddid_value; /* 24-bit RDDID with the dummy bit removed */
    bool is_st7796;
} display_id_t;

/* Reads the controller ID over MISO, then brings up the panel and LVGL. */
esp_err_t display_init(display_id_t *id_out, lv_display_t **disp_out);

/* Blocks until every queued pixel transfer has reached the panel. */
void display_wait_idle(void);
