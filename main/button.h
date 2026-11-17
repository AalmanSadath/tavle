#pragma once

#include "esp_err.h"

/* Push button on TAVLE_PIN_BUTTON to GND: a short press shows the next
 * screen, holding it for two seconds reboots once it is let go. */
esp_err_t button_start(void);
