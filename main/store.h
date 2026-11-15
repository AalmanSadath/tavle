#pragma once

#include <stddef.h>

#include "esp_err.h"

#define STORE_MAX_LAYOUT (16 * 1024)

esp_err_t store_init(void);

/* Returns the newest valid saved layout (malloc'd, NUL-terminated), or NULL. */
char *store_load(size_t *len);

/* Saves a layout into the older of the two slots. */
esp_err_t store_save(const char *data, size_t len);
