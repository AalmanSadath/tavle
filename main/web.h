#pragma once

#include "esp_err.h"

typedef void (*web_reload_cb_t)(void);

/* Starts the setup page and API on port 80. */
esp_err_t web_start(web_reload_cb_t on_reload);
