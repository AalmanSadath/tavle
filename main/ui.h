#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "fetch.h"
#include "lvgl.h"
#include "net.h"
#include "png.h"

/* Starts the UI task, which owns the layout and every screen. */
esp_err_t ui_start(lv_display_t *disp);

/* Safe from any task: hand a network change or a reload to the UI task. */
void ui_net_status(const net_status_t *status);
void ui_request_reload(void);

/* Safe from any task: go to the next screen (button, setup page). */
void ui_next_screen(void);

/* Safe from any task: a setting the UI uses has changed. */
void ui_settings_changed(void);

typedef struct {
    char name[24];      /* visible screen */
    int index, count;   /* index from 0 */
    bool keep_running;  /* value in use */
    const char *keep_from; /* "setting", "layout" or "board default" */
} ui_screen_info_t;

void ui_get_screen_info(ui_screen_info_t *out);

/* Hands a layout to the UI task, which parses it and shows it if usable.
 * Takes ownership of json (malloc'd). */
void ui_set_layout(char *json, size_t len, layout_src_t src);

/* From source drivers, any task. Never blocks for long. */
void ui_source_value(unsigned gen, int index, const char *path, bool is_num, double num,
                     const char *text);
void ui_source_state(unsigned gen, int index, bool online);

/* From the fetch task: a picture asked for with fetch_image(). Takes
 * ownership of url and pic; pic == NULL when it could not be loaded. */
void ui_image_ready(unsigned gen, int slot, char *url, png_picture_t *pic);
