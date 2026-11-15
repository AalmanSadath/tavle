#pragma once

#include "layout.h"
#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

typedef struct {
    const layout_widget_t *def;
    lv_obj_t *cell;
    lv_obj_t *value;  /* main text: number, text, symbol or time */
    lv_obj_t *caption;
    lv_obj_t *detail; /* progress: text line from the second path; value: unit */
    lv_obj_t *shape;  /* bar, arc or chart */
    int32_t fit_w;    /* value: room for the number, for shrinking long text */
    lv_chart_series_t *series[LAYOUT_MAX_SERIES];
    /* live state, owned by the UI task */
    double last[LAYOUT_MAX_SERIES];   /* latest number per path, for graph sampling */
    bool has_last[LAYOUT_MAX_SERIES];
    bool stale;               /* source offline: shown dimmed */
    int64_t next_sample_us;   /* graph: when the next point is due */
} widget_view_t;

typedef struct {
    lv_obj_t *screen;
    widget_view_t widgets[LAYOUT_MAX_WIDGETS];
    int count;
} screen_view_t;

/* Builds one LVGL screen per layout screen. Call with the LVGL lock held. */
void widgets_build(lv_display_t *disp, const layout_t *layout, screen_view_t *views);

void widget_set_number(widget_view_t *w, double v);
void widget_set_text(widget_view_t *w, const char *s);

/* Progress: the text line under the caption. */
void widget_set_detail(widget_view_t *w, const char *s);

/* Adds a point to one line of a graph. */
void widget_graph_push(widget_view_t *w, int series, double v);

/* Shows "--" everywhere until a source delivers a value. */
void widgets_reset(screen_view_t *view);

/* Dims a widget whose source is offline; its last value stays visible. */
void widget_set_stale(widget_view_t *w, bool stale);

/* Updates clock widgets; now == NULL before the clock is set. */
struct tm;
void widgets_update_clocks(screen_view_t *view, const struct tm *now);

/* Changes every widget on the screen, for the refresh benchmark. */
void widgets_bench_step(screen_view_t *view, int frame);

/* Deletes the screens built by widgets_build. Call with the LVGL lock held,
 * after another screen has been loaded. */
void widgets_destroy(screen_view_t *views, int count);
