#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    NET_SETUP,      /* no saved network: setup hotspot only */
    NET_CONNECTING, /* joining the saved network */
    NET_CONNECTED,  /* station has an IP */
    NET_FALLBACK,   /* saved network failed for a while: hotspot up, station still retrying */
} net_state_t;

typedef struct {
    net_state_t state;
    bool ap_active;
    char ssid[33];     /* saved network */
    char ip[16];       /* station IP when connected */
    char ap_ssid[16];  /* setup hotspot */
    char ap_pass[9];   /* setup hotspot password, shown on screen only */
    char hostname[24]; /* mDNS name without ".local", "" before the first join */
    int8_t rssi;
} net_status_t;

typedef void (*net_status_cb_t)(const net_status_t *status);

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;
} net_scan_entry_t;

/* Starts Wi-Fi with the saved settings. cb runs on the net task on every change. */
esp_err_t net_start(net_status_cb_t cb);

/* Copies the current status. */
void net_get_status(net_status_t *out);

/* Rejoins with the credentials now in settings. Returns at once. */
void net_apply_settings(void);

/* Blocking scan, unique SSIDs strongest first. Returns the number written. */
int net_scan(net_scan_entry_t *out, int max);

const char *net_state_name(net_state_t state);
