#include "widgets.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define GAP          6
#define GRAPH_POINTS 30

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_28);

static const int32_t s_cols[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
static const int32_t s_rows[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                 LV_GRID_TEMPLATE_LAST};

static lv_style_t s_cell;
static lv_style_t s_caption;

static void styles_init(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;

    lv_style_init(&s_cell);
    lv_style_set_bg_color(&s_cell, lv_color_hex(0x1c1c1e));
    lv_style_set_bg_opa(&s_cell, LV_OPA_COVER);
    lv_style_set_radius(&s_cell, 8);
    lv_style_set_border_width(&s_cell, 0);
    lv_style_set_pad_all(&s_cell, 6);
    lv_style_set_pad_row(&s_cell, 2);

    lv_style_init(&s_caption);
    lv_style_set_text_color(&s_caption, lv_color_hex(0x8e8e93));
    lv_style_set_text_font(&s_caption, &lv_font_montserrat_14);
}

static lv_obj_t *add_label(lv_obj_t *parent, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_text(l, "");
    return l;
}

static void add_caption(widget_view_t *w, const char *fallback)
{
    w->caption = add_label(w->cell, &lv_font_montserrat_14);
    lv_obj_add_style(w->caption, &s_caption, 0);
    lv_label_set_text(w->caption, w->def->label ? w->def->label : fallback);
}

static void build_widget(lv_obj_t *grid, widget_view_t *w)
{
    const layout_widget_t *d = w->def;

    w->cell = lv_obj_create(grid);
    lv_obj_remove_style_all(w->cell);
    lv_obj_add_style(w->cell, &s_cell, 0);
    lv_obj_remove_flag(w->cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_cell(w->cell, LV_GRID_ALIGN_STRETCH, d->col, d->colspan,
                         LV_GRID_ALIGN_STRETCH, d->row, d->rowspan);
    lv_obj_set_flex_flow(w->cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(w->cell, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    switch (d->kind) {
    case WIDGET_TEXT:
        if (d->label) {
            add_caption(w, "");
        }
        w->value = add_label(w->cell, &lv_font_montserrat_20);
        lv_obj_set_width(w->value, lv_pct(100));
        lv_label_set_long_mode(w->value, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(w->value, LV_TEXT_ALIGN_CENTER, 0);
        break;

    case WIDGET_VALUE: {
        add_caption(w, d->path);
        lv_obj_t *row = lv_obj_create(w->cell);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_column(row, 2, 0);
        w->value = add_label(row, &lv_font_montserrat_28);
        if (d->unit) {
            lv_obj_t *unit = add_label(row, &lv_font_montserrat_14);
            lv_obj_add_style(unit, &s_caption, 0);
            lv_label_set_text(unit, d->unit);
        }
        break;
    }

    case WIDGET_PROGRESS:
        add_caption(w, "progress");
        w->value = add_label(w->cell, &lv_font_montserrat_20);
        w->shape = lv_bar_create(w->cell);
        lv_obj_set_size(w->shape, lv_pct(100), 14);
        lv_bar_set_range(w->shape, 0, 1000);
        break;

    case WIDGET_GAUGE:
        add_caption(w, d->path);
        w->shape = lv_arc_create(w->cell);
        lv_obj_set_size(w->shape, 72, 72);
        lv_arc_set_rotation(w->shape, 135);
        lv_arc_set_bg_angles(w->shape, 0, 270);
        lv_arc_set_range(w->shape, 0, 1000);
        lv_obj_remove_style(w->shape, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(w->shape, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(w->shape, 8, LV_PART_MAIN);
        lv_obj_set_style_arc_width(w->shape, 8, LV_PART_INDICATOR);
        w->value = add_label(w->shape, &lv_font_montserrat_14);
        lv_obj_center(w->value);
        break;

    case WIDGET_GRAPH:
        add_caption(w, d->label ? d->label : d->path);
        w->shape = lv_chart_create(w->cell);
        lv_obj_set_width(w->shape, lv_pct(100));
        lv_obj_set_flex_grow(w->shape, 1);
        lv_chart_set_type(w->shape, LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(w->shape, GRAPH_POINTS);
        lv_chart_set_div_line_count(w->shape, 3, 0);
        lv_chart_set_range(w->shape, LV_CHART_AXIS_PRIMARY_Y, (int32_t)d->min, (int32_t)d->max);
        lv_obj_set_style_size(w->shape, 0, 0, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(w->shape, 2, 0);
        w->series = lv_chart_add_series(w->shape, lv_palette_main(LV_PALETTE_ORANGE),
                                        LV_CHART_AXIS_PRIMARY_Y);
        break;

    case WIDGET_ICON:
        w->value = add_label(w->cell, &lv_font_montserrat_28);
        w->caption = add_label(w->cell, &lv_font_montserrat_14);
        lv_obj_add_style(w->caption, &s_caption, 0);
        break;

    case WIDGET_CLOCK:
        if (d->label) {
            add_caption(w, "");
        }
        w->value = add_label(w->cell, &lv_font_montserrat_28);
        break;
    }
}

void widgets_build(lv_display_t *disp, const layout_t *layout, screen_view_t *views)
{
    styles_init();
    lv_theme_t *theme = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
                                              lv_palette_main(LV_PALETTE_ORANGE), true,
                                              &lv_font_montserrat_14);
    lv_display_set_theme(disp, theme);

    for (int s = 0; s < layout->screen_count; s++) {
        const layout_screen_t *ls = &layout->screens[s];
        screen_view_t *v = &views[s];

        v->screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(v->screen, lv_color_black(), 0);
        lv_obj_remove_flag(v->screen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *grid = lv_obj_create(v->screen);
        lv_obj_remove_style_all(grid);
        lv_obj_set_size(grid, lv_pct(100), lv_pct(100));
        lv_obj_set_style_pad_all(grid, GAP, 0);
        lv_obj_set_style_pad_row(grid, GAP, 0);
        lv_obj_set_style_pad_column(grid, GAP, 0);
        lv_obj_set_grid_dsc_array(grid, s_cols, s_rows);
        lv_obj_set_layout(grid, LV_LAYOUT_GRID);
        lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

        v->count = ls->widget_count;
        for (int i = 0; i < ls->widget_count; i++) {
            v->widgets[i].def = &ls->widgets[i];
            build_widget(grid, &v->widgets[i]);
        }
    }
}

static double fraction(const layout_widget_t *d, double v)
{
    double f = (v - d->min) / (d->max - d->min);
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

static void format_number(const layout_widget_t *d, const char *def_fmt, double v, char *buf,
                          size_t len)
{
    /* d->format passed number_format_ok(): exactly one double conversion. */
    snprintf(buf, len, d->format ? d->format : def_fmt, v);
}

void widget_set_number(widget_view_t *w, double v)
{
    const layout_widget_t *d = w->def;
    char buf[32];

    switch (d->kind) {
    case WIDGET_VALUE:
        format_number(d, "%.1f", v, buf, sizeof(buf));
        lv_label_set_text(w->value, buf);
        break;
    case WIDGET_PROGRESS:
    case WIDGET_GAUGE: {
        double f = fraction(d, v);
        if (d->format) {
            format_number(d, NULL, v, buf, sizeof(buf));
        } else {
            snprintf(buf, sizeof(buf), "%.0f%%", f * 100);
        }
        lv_label_set_text(w->value, buf);
        if (d->kind == WIDGET_PROGRESS) {
            lv_bar_set_value(w->shape, (int32_t)(f * 1000), LV_ANIM_OFF);
        } else {
            lv_arc_set_value(w->shape, (int32_t)(f * 1000));
        }
        break;
    }
    case WIDGET_GRAPH:
        lv_chart_set_next_value(w->shape, w->series, (int32_t)lround(v));
        break;
    case WIDGET_TEXT:
        format_number(d, "%g", v, buf, sizeof(buf));
        lv_label_set_text(w->value, buf);
        break;
    case WIDGET_ICON:
    case WIDGET_CLOCK:
        break;
    }
}

static const struct {
    const char *state;
    const char *symbol;
} ICONS[] = {
    {"printing", LV_SYMBOL_PLAY},
    {"paused", LV_SYMBOL_PAUSE},
    {"complete", LV_SYMBOL_OK},
    {"standby", LV_SYMBOL_HOME},
    {"error", LV_SYMBOL_WARNING},
    {"cancelled", LV_SYMBOL_CLOSE},
    {"on", LV_SYMBOL_POWER},
    {"off", LV_SYMBOL_POWER},
};

void widget_set_text(widget_view_t *w, const char *s)
{
    switch (w->def->kind) {
    case WIDGET_ICON: {
        const char *sym = LV_SYMBOL_DUMMY "?";
        for (size_t i = 0; i < sizeof(ICONS) / sizeof(ICONS[0]); i++) {
            if (strcmp(s, ICONS[i].state) == 0) {
                sym = ICONS[i].symbol;
                break;
            }
        }
        lv_label_set_text(w->value, sym);
        lv_label_set_text(w->caption, s);
        break;
    }
    case WIDGET_TEXT:
    case WIDGET_VALUE:
    case WIDGET_CLOCK:
        lv_label_set_text(w->value, s);
        break;
    default:
        break;
    }
}

static void set_clock(widget_view_t *w, int hour, int min)
{
    struct tm t = {.tm_hour = hour, .tm_min = min, .tm_mday = 1, .tm_year = 126};
    char buf[24];
    if (strftime(buf, sizeof(buf), w->def->format, &t) == 0) {
        buf[0] = '\0';
    }
    lv_label_set_text(w->value, buf);
}

static const struct {
    const char *path;
    double number;
    const char *text;
} PLACEHOLDERS[] = {
    {"print_stats.filename", 0, "tavle_test.gcode"},
    {"print_stats.state", 0, "printing"},
    {"extruder.temperature", 215.3, NULL},
    {"heater_bed.temperature", 60.1, NULL},
    {"fan.speed", 0.62, NULL},
    {"display_status.progress", 0.42, NULL},
};

static void fill_one(widget_view_t *w)
{
    const layout_widget_t *d = w->def;
    if (d->kind == WIDGET_CLOCK) {
        set_clock(w, 12, 34);
        return;
    }

    double number = (d->min + d->max) / 2;
    const char *text = "tavle";
    for (size_t i = 0; i < sizeof(PLACEHOLDERS) / sizeof(PLACEHOLDERS[0]); i++) {
        if (strcmp(d->path, PLACEHOLDERS[i].path) == 0) {
            number = PLACEHOLDERS[i].number;
            text = PLACEHOLDERS[i].text;
            break;
        }
    }

    if (d->kind == WIDGET_GRAPH) {
        double span = d->max - d->min;
        for (int i = 0; i < GRAPH_POINTS; i++) {
            double v = number + span * 0.15 * sin(i * 0.4);
            widget_set_number(w, v);
        }
    } else if (text && (d->kind == WIDGET_TEXT || d->kind == WIDGET_ICON)) {
        widget_set_text(w, text);
    } else if (d->kind == WIDGET_ICON) {
        widget_set_text(w, "printing");
    } else {
        widget_set_number(w, number);
    }
}

void widgets_fill_placeholders(screen_view_t *view)
{
    for (int i = 0; i < view->count; i++) {
        fill_one(&view->widgets[i]);
    }
}

void widgets_bench_step(screen_view_t *view, int frame)
{
    static const char *const states[] = {"printing", "paused", "complete", "standby"};
    for (int i = 0; i < view->count; i++) {
        widget_view_t *w = &view->widgets[i];
        const layout_widget_t *d = w->def;
        double f = (frame % 100) / 100.0;
        double v = d->min + (d->max - d->min) * f;
        char buf[24];

        switch (d->kind) {
        case WIDGET_TEXT:
            snprintf(buf, sizeof(buf), "frame %d", frame);
            widget_set_text(w, buf);
            break;
        case WIDGET_ICON:
            widget_set_text(w, states[(frame / 10) % 4]);
            break;
        case WIDGET_CLOCK:
            set_clock(w, (frame / 60) % 24, frame % 60);
            break;
        default:
            widget_set_number(w, v);
            break;
        }
    }
}

void widgets_destroy(screen_view_t *views, int count)
{
    for (int i = 0; i < count; i++) {
        if (views[i].screen) {
            lv_obj_delete(views[i].screen);
        }
        memset(&views[i], 0, sizeof(views[i]));
    }
}
