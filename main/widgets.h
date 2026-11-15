#pragma once

#include "layout.h"
#include "lvgl.h"

typedef struct {
    const layout_widget_t *def;
    lv_obj_t *cell;
    lv_obj_t *value;  /* main text: number, text, symbol or time */
    lv_obj_t *caption;
    lv_obj_t *shape;  /* bar, arc or chart */
    lv_chart_series_t *series;
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

/* Fixed values until data sources exist. */
void widgets_fill_placeholders(screen_view_t *view);

/* Changes every widget on the screen, for the refresh benchmark. */
void widgets_bench_step(screen_view_t *view, int frame);
