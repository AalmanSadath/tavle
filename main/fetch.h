#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    LAYOUT_SRC_BUILTIN,
    LAYOUT_SRC_SAVED,
    LAYOUT_SRC_FETCHED,
} layout_src_t;

typedef struct {
    layout_src_t source;  /* what is on screen */
    size_t bytes;         /* its size */
    int64_t last_ok_us;   /* esp_timer time of the last good fetch, 0 = never */
    char error[80];       /* last fetch error, "" after a good fetch */
} layout_status_t;

esp_err_t fetch_start(void);

/* Fetch now (reload button). Without a URL, rebuilds from the saved or
 * built-in layout instead. */
void fetch_trigger(void);

/* The layout URL or interval changed: restart the timer and fetch now. */
void fetch_settings_changed(void);

/* The station got an IP. */
void fetch_network_up(void);

/* Called by the UI task. */
void fetch_report_shown(layout_src_t source, size_t bytes);
void fetch_report_result(bool ok, const char *error);

void fetch_get_status(layout_status_t *out);
const char *layout_src_name(layout_src_t src);

/* Downloads a PNG from an http:// url and shrinks it to fit max_w x max_h, blending
 * transparency onto bg (0xRRGGBB). The result goes to ui_image_ready() with
 * the same gen and slot. A newer request for a slot replaces one that has
 * not started yet. */
void fetch_image(unsigned gen, int slot, const char *url, int max_w, int max_h, uint32_t bg);
