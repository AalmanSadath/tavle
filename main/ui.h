#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "net.h"

/* Starts the UI task, which owns the layout and every screen. */
esp_err_t ui_start(lv_display_t *disp);

/* Safe from any task: hand a network change or a reload to the UI task. */
void ui_net_status(const net_status_t *status);
void ui_request_reload(void);
