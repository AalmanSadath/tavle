#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

/* Hardware config: how the board is wired. Read from flash at boot, so one
 * firmware per chip variant runs any wiring and display. Written by
 * tools/tavle.py or the setup page; without one the variant's reference
 * wiring is used. Stored as JSON:
 *
 *   { "board": "ESP32-C3 reference",
 *     "display": { "driver": "st7796", "spi_mhz": 40, "bgr": true,
 *                  "invert": false, "mirror_x": true, "mirror_y": false },
 *     "pins": { "sck": 6, "mosi": 7, "miso": 5, "cs": 10, "dc": 4,
 *               "rst": 3, "bl": 1, "button": 0 },
 *     "wifi_tx_power_qdbm": 34 }
 *
 * sck, mosi, cs and dc are required; a missing miso, rst, bl or button (or
 * -1) means not connected. */

#define HWCFG_NAME_MAX   40
#define HWCFG_DRIVER_MAX 15
#define HWCFG_JSON_MAX   (4096 - 16) /* one flash sector less the header */

typedef struct {
    char board[HWCFG_NAME_MAX + 1];
    char display[HWCFG_DRIVER_MAX + 1]; /* driver name, see display.c */
    int spi_mhz;
    bool bgr, invert, mirror_x, mirror_y;
    int sck, mosi, miso, cs, dc, rst, bl; /* -1: not connected */
    int button;                           /* -1: no button */
    int tx_power_qdbm;                    /* 0: chip default */
} hwcfg_t;

/* Reads the saved config, or falls back to the variant's default. Call
 * before the display and the button start. */
void hwcfg_load(void);

const hwcfg_t *hwcfg(void);

/* true when the saved config is in use, false for the variant default. */
bool hwcfg_is_saved(void);

/* Why the saved config is not in use, "" when it is or none was saved. */
const char *hwcfg_problem(void);

/* Parses and checks a config. On failure err holds the reason. */
esp_err_t hwcfg_parse(const char *json, size_t len, hwcfg_t *out, char *err, size_t errlen);

/* Checks, then saves a config for the next boot. */
esp_err_t hwcfg_save(const char *json, size_t len, char *err, size_t errlen);

/* The config as a JSON object, in the stored form. */
cJSON *hwcfg_to_json(const hwcfg_t *c);
