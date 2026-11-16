#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SETTINGS_SSID_MAX       32
#define SETTINGS_WIFI_PASS_MAX  63
#define SETTINGS_URL_MAX        256
#define SETTINGS_ADMIN_MIN      8
#define SETTINGS_ADMIN_MAX      64
#define SETTINGS_INTERVAL_MIN   1
#define SETTINGS_INTERVAL_MAX   1440
#define SETTINGS_INTERVAL_DEF   5
#define SETTINGS_TZ_MAX         48
#define SETTINGS_HOST_MAX       20  /* leaves room for a "-9" suffix */

typedef enum {
    KEEP_AUTO, /* the layout's "keep_running", else the board default */
    KEEP_ON,
    KEEP_OFF,
} keep_running_t;

typedef struct {
    char ssid[SETTINGS_SSID_MAX + 1];
    char wifi_pass[SETTINGS_WIFI_PASS_MAX + 1];
    char layout_url[SETTINGS_URL_MAX + 1];
    uint16_t interval_min;
    char tz[SETTINGS_TZ_MAX + 1];  /* POSIX TZ string */
    keep_running_t keep_running;   /* keep other screens running */
    char hostname[SETTINGS_HOST_MAX + 1]; /* mDNS base name, "" = tavle */
    bool has_admin;
    uint8_t admin_salt[16];
    uint8_t admin_hash[32];
} settings_t;

/* Initialises NVS and loads the stored settings. */
esp_err_t settings_init(void);

/* Copies the current settings. */
void settings_get(settings_t *out);

/* Stores new settings in NVS and makes them current. */
esp_err_t settings_save(const settings_t *s);

/* Sets a new admin password on s (salted hash). NULL or "" clears it. */
void settings_set_admin(settings_t *s, const char *password);

/* True when no admin password is set, or when password matches it. */
bool settings_check_admin(const char *password);

bool settings_has_admin(void);

/* True for 1 to 20 of a-z, 0-9 and "-", not starting or ending with "-". */
bool settings_valid_hostname(const char *name);

const char *keep_running_name(keep_running_t k);

/* Parses "auto", "on" or "off"; false for anything else. */
bool keep_running_from_name(const char *s, keep_running_t *out);
