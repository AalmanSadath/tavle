#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "fetch.h"
#include "lvgl.h"
#include "net.h"

/* Starts the UI task, which owns the layout and every screen. */
esp_err_t ui_start(lv_display_t *disp);

/* Safe from any task: hand a network change or a reload to the UI task. */
void ui_net_status(const net_status_t *status);
void ui_request_reload(void);

/* Hands a layout to the UI task, which parses it and shows it if usable.
 * Takes ownership of json (malloc'd). */
void ui_set_layout(char *json, size_t len, layout_src_t src);

/* From source drivers, any task. Never blocks for long. */
void ui_source_value(unsigned gen, int index, const char *path, bool is_num, double num,
                     const char *text);
void ui_source_state(unsigned gen, int index, bool online);
