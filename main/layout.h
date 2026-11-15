#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

#define LAYOUT_COLS        3
#define LAYOUT_ROWS        4
#define LAYOUT_MAX_SOURCES 16
#define LAYOUT_MAX_SCREENS 8
#define LAYOUT_MAX_WIDGETS (LAYOUT_COLS * LAYOUT_ROWS)
#define LAYOUT_MAX_SERIES  3 /* lines in one graph */

typedef enum {
    SOURCE_HTTP_JSON,
    SOURCE_MOONRAKER,
    SOURCE_MQTT,
} source_type_t;

typedef struct {
    const char *name;
    source_type_t type;
    const cJSON *config; /* the source object, read by the source driver */
} layout_source_t;

typedef enum {
    WIDGET_TEXT,
    WIDGET_VALUE,
    WIDGET_PROGRESS,
    WIDGET_GAUGE,
    WIDGET_GRAPH,
    WIDGET_ICON,
    WIDGET_CLOCK,
} widget_kind_t;

typedef struct {
    widget_kind_t kind;
    int col, row, colspan, rowspan;
    const char *source; /* NULL for clock */
    const char *path;   /* == paths[0] */
    const char *paths[LAYOUT_MAX_SERIES]; /* graph: one line per path; others: just path */
    const char *series_labels[LAYOUT_MAX_SERIES];
    int path_count;
    const char *format; /* printf for numbers, strftime for clock; validated */
    const char *unit;
    const char *label;
    double min, max;
    int period_s; /* graph: seconds between samples */
} layout_widget_t;

typedef struct {
    const char *name;
    layout_widget_t widgets[LAYOUT_MAX_WIDGETS];
    int widget_count;
} layout_screen_t;

typedef struct {
    cJSON *root; /* owns every string referenced below */
    layout_source_t sources[LAYOUT_MAX_SOURCES];
    int source_count;
    layout_screen_t screens[LAYOUT_MAX_SCREENS];
    int screen_count;
} layout_t;

/* Parses layout JSON. Invalid widgets and sources are logged and skipped;
 * returns false only when nothing usable is left. */
bool layout_parse(const char *json, size_t len, layout_t *out);
void layout_free(layout_t *layout);

/* Why the last layout_parse returned false. */
const char *layout_last_error(void);

const char *widget_kind_name(widget_kind_t kind);
