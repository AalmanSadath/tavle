#include "widgets.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GAP          6
#define CELL_BG      0x1c1c1e
#define GRAPH_POINTS 30
#define SERIES_COLORS ((const lv_color_t[]){lv_color_hex(0xff9f0a), lv_color_hex(0x0a84ff), \
                                             lv_color_hex(0x30d158)})

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_28);

static const int32_t s_cols[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
static const int32_t s_rows[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                 LV_GRID_TEMPLATE_LAST};

static void set_label(lv_obj_t *l, const char *text);

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
    lv_style_set_bg_color(&s_cell, lv_color_hex(CELL_BG));
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
            w->detail = add_label(row, &lv_font_montserrat_14); /* the unit */
            lv_obj_add_style(w->detail, &s_caption, 0);
            lv_label_set_text(w->detail, d->unit);
        }
        break;
    }

    case WIDGET_PROGRESS:
        add_caption(w, "progress");
        if (d->path_count > 1) {
            w->detail = add_label(w->cell, &lv_font_montserrat_14);
            lv_obj_set_width(w->detail, lv_pct(100));
            lv_label_set_long_mode(w->detail, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(w->detail, LV_TEXT_ALIGN_CENTER, 0);
        }
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
        if (d->path_count > 1) {
            /* legend: optional label, then one coloured name per line */
            lv_obj_t *row = lv_obj_create(w->cell);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(row, 10, 0);
            if (d->label) {
                lv_obj_t *l = add_label(row, &lv_font_montserrat_14);
                lv_obj_add_style(l, &s_caption, 0);
                lv_label_set_text(l, d->label);
            }
            for (int i = 0; i < d->path_count; i++) {
                lv_obj_t *l = add_label(row, &lv_font_montserrat_14);
                lv_obj_set_style_text_color(l, SERIES_COLORS[i], 0);
                char buf[40];
                snprintf(buf, sizeof(buf), "\xe2\x80\xa2 %s",
                         d->series_labels[i] ? d->series_labels[i] : d->paths[i]);
                lv_label_set_text(l, buf);
            }
        } else {
            add_caption(w, d->label ? d->label : d->path);
        }
        w->shape = lv_chart_create(w->cell);
        lv_obj_set_width(w->shape, lv_pct(100));
        lv_obj_set_flex_grow(w->shape, 1);
        lv_chart_set_type(w->shape, LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(w->shape, GRAPH_POINTS);
        lv_chart_set_div_line_count(w->shape, 3, 0);
        lv_chart_set_range(w->shape, LV_CHART_AXIS_PRIMARY_Y, (int32_t)d->min, (int32_t)d->max);
        lv_obj_set_style_size(w->shape, 0, 0, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(w->shape, 2, 0);
        for (int i = 0; i < d->path_count; i++) {
            w->series[i] = lv_chart_add_series(w->shape, SERIES_COLORS[i], LV_CHART_AXIS_PRIMARY_Y);
        }
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

    case WIDGET_IMAGE:
        if (d->label) {
            add_caption(w, "");
        }
        /* holds one image per strip of the picture */
        w->shape = lv_obj_create(w->cell);
        lv_obj_remove_style_all(w->shape);
        lv_obj_set_size(w->shape, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(w->shape, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(w->shape, LV_OBJ_FLAG_HIDDEN);
        w->value = add_label(w->cell, &lv_font_montserrat_20); /* "--" while there is none */
        break;
    }
}

/* After layout: how wide a value may get before it is shrunk. */
static void measure(widget_view_t *w)
{
    if (w->def->kind == WIDGET_VALUE) {
        w->fit_w = lv_obj_get_content_width(w->cell);
        if (w->detail) {
            w->fit_w -= lv_obj_get_width(w->detail) + 2;
        }
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
        lv_obj_update_layout(v->screen);
        for (int i = 0; i < v->count; i++) {
            measure(&v->widgets[i]);
        }
    }
}

static int32_t text_width(const char *text, const lv_font_t *font)
{
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* A value too wide for its cell drops to a smaller font, and then, for
 * "12/340" style text, to the part before the slash. */
static void set_value_text(widget_view_t *w, const char *text)
{
    static const lv_font_t *const fonts[] = {&lv_font_montserrat_28, &lv_font_montserrat_20};
    char buf[64];
    strlcpy(buf, text, sizeof(buf));
    const lv_font_t *font = fonts[1];
    bool fitted = false;
    for (int pass = 0; pass < 2 && !fitted; pass++) {
        for (int f = 0; f < 2 && !fitted; f++) {
            if (text_width(buf, fonts[f]) <= w->fit_w) {
                font = fonts[f];
                fitted = true;
            }
        }
        char *slash = strchr(buf, '/');
        if (fitted || !slash) {
            break;
        }
        *slash = '\0';
    }
    if (lv_obj_get_style_text_font(w->value, 0) != font) {
        lv_obj_set_style_text_font(w->value, font, 0);
    }
    set_label(w->value, buf);
}

static double fraction(const layout_widget_t *d, double v)
{
    double f = (v - d->min) / (d->max - d->min);
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

static void format_duration(double v, char *buf, size_t len)
{
    if (v < 0) {
        v = 0;
    }
    long s = lround(v);
    if (s >= 3600) {
        snprintf(buf, len, "%ldh %02ldm", s / 3600, (s % 3600) / 60);
    } else {
        snprintf(buf, len, "%ld:%02ld", s / 60, s % 60);
    }
}

static void format_number(const layout_widget_t *d, const char *def_fmt, double v, char *buf,
                          size_t len)
{
    if (d->format && strcmp(d->format, "duration") == 0) {
        format_duration(v, buf, len);
        return;
    }
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
        set_value_text(w, buf);
        break;
    case WIDGET_PROGRESS:
    case WIDGET_GAUGE: {
        double f = fraction(d, v);
        if (d->format) {
            format_number(d, NULL, v, buf, sizeof(buf));
        } else {
            snprintf(buf, sizeof(buf), "%.0f%%", f * 100);
        }
        set_label(w->value, buf);
        if (d->kind == WIDGET_PROGRESS) {
            lv_bar_set_value(w->shape, (int32_t)(f * 1000), LV_ANIM_OFF);
        } else {
            lv_arc_set_value(w->shape, (int32_t)(f * 1000));
        }
        break;
    }
    case WIDGET_GRAPH:
        widget_graph_push(w, 0, v);
        break;
    case WIDGET_TEXT:
        format_number(d, "%g", v, buf, sizeof(buf));
        set_label(w->value, buf);
        break;
    case WIDGET_ICON:
    case WIDGET_CLOCK:
    case WIDGET_IMAGE:
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
        set_label(w->value, sym);
        set_label(w->caption, s);
        break;
    }
    case WIDGET_VALUE:
        set_value_text(w, s);
        break;
    case WIDGET_TEXT:
    case WIDGET_CLOCK:
        set_label(w->value, s);
        break;
    default:
        break;
    }
}

void widget_set_detail(widget_view_t *w, const char *s)
{
    if (w->def->kind == WIDGET_PROGRESS && w->detail) {
        set_label(w->detail, s);
    }
}

void widget_image_box(const widget_view_t *w, int *max_w, int *max_h, uint32_t *bg)
{
    *max_w = lv_obj_get_content_width(w->cell);
    *max_h = lv_obj_get_content_height(w->cell);
    if (w->caption) {
        *max_h -= lv_obj_get_height(w->caption) + lv_obj_get_style_pad_row(w->cell, 0);
    }
    *bg = CELL_BG;
}

/* A picture is shown as one image per strip, stacked without gaps. */
struct picture {
    png_picture_t *pic;
    lv_image_dsc_t dsc[PNG_MAX_STRIPS];
};

static void free_image(widget_view_t *w)
{
    if (w->img) {
        for (int i = 0; i < w->img->pic->strips; i++) {
            lv_image_cache_drop(&w->img->dsc[i]);
        }
        png_picture_free(w->img->pic);
        free(w->img);
        w->img = NULL;
    }
}

void widget_set_image(widget_view_t *w, png_picture_t *pic)
{
    struct picture *img = NULL;
    if (w->def->kind != WIDGET_IMAGE || (pic && !(img = calloc(1, sizeof(*img))))) {
        png_picture_free(pic);
        pic = NULL;
    }
    lv_obj_clean(w->shape); /* the old strips */
    if (img) {
        img->pic = pic;
        for (int i = 0; i < pic->strips; i++) {
            int rows = pic->h - i * pic->strip_rows;
            rows = rows < pic->strip_rows ? rows : pic->strip_rows;
            lv_image_dsc_t *d = &img->dsc[i];
            d->header.magic = LV_IMAGE_HEADER_MAGIC;
            d->header.cf = LV_COLOR_FORMAT_RGB565;
            d->header.w = pic->w;
            d->header.h = rows;
            d->header.stride = pic->w * 2;
            d->data_size = (uint32_t)pic->w * rows * 2;
            d->data = (const uint8_t *)pic->px[i];
            lv_obj_t *strip = lv_image_create(w->shape);
            lv_image_set_src(strip, d);
            if (w->stale) {
                lv_obj_set_style_image_opa(strip, LV_OPA_40, 0);
            }
        }
        lv_obj_remove_flag(w->shape, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(w->value, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(w->shape, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(w->value, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(w->value, "--");
    }
    free_image(w); /* the old one, now that nothing shows it */
    w->img = img;
}

void widget_graph_push(widget_view_t *w, int series, double v)
{
    if (w->def->kind == WIDGET_GRAPH && series < w->def->path_count && w->series[series]) {
        lv_chart_set_next_value(w->shape, w->series[series], (int32_t)lround(v));
    }
}

static void set_label(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text); /* unchanged text would still redraw */
    }
}

static void set_clock(widget_view_t *w, int hour, int min)
{
    struct tm t = {.tm_hour = hour, .tm_min = min, .tm_mday = 1, .tm_year = 126};
    char buf[24];
    if (strftime(buf, sizeof(buf), w->def->format, &t) == 0) {
        buf[0] = '\0';
    }
    set_label(w->value, buf);
}

void widgets_update_clocks(screen_view_t *view, const struct tm *now)
{
    for (int i = 0; i < view->count; i++) {
        widget_view_t *w = &view->widgets[i];
        if (w->def->kind != WIDGET_CLOCK) {
            continue;
        }
        char buf[24] = "--:--";
        if (now && strftime(buf, sizeof(buf), w->def->format, now) == 0) {
            buf[0] = '\0';
        }
        set_label(w->value, buf);
    }
}

void widgets_reset(screen_view_t *view)
{
    for (int i = 0; i < view->count; i++) {
        widget_view_t *w = &view->widgets[i];
        memset(w->has_last, 0, sizeof(w->has_last));
        w->stale = false;
        w->next_sample_us = 0;
        switch (w->def->kind) {
        case WIDGET_TEXT:
            lv_label_set_text(w->value, "--");
            break;
        case WIDGET_VALUE:
            set_value_text(w, "--");
            break;
        case WIDGET_PROGRESS:
            lv_bar_set_value(w->shape, 0, LV_ANIM_OFF);
            lv_label_set_text(w->value, "--");
            break;
        case WIDGET_GAUGE:
            lv_arc_set_value(w->shape, 0);
            lv_label_set_text(w->value, "--");
            break;
        case WIDGET_GRAPH:
            for (int k = 0; k < w->def->path_count; k++) {
                lv_chart_set_all_value(w->shape, w->series[k], LV_CHART_POINT_NONE);
            }
            break;
        case WIDGET_ICON:
            lv_label_set_text(w->value, "--");
            lv_label_set_text(w->caption, "");
            break;
        case WIDGET_CLOCK:
            lv_label_set_text(w->value, "--:--");
            break;
        case WIDGET_IMAGE:
            widget_set_image(w, NULL);
            free(w->img_url);
            w->img_url = NULL;
            w->img_retry_us = 0;
            break;
        }
        if (w->detail && w->def->kind == WIDGET_PROGRESS) {
            lv_label_set_text(w->detail, "");
        }
    }
}

void widget_set_stale(widget_view_t *w, bool stale)
{
    if (w->stale == stale || w->def->kind == WIDGET_CLOCK) {
        return;
    }
    w->stale = stale;
    lv_opa_t opa = stale ? LV_OPA_40 : LV_OPA_COVER;
    if (w->value) {
        lv_obj_set_style_text_opa(w->value, opa, 0);
    }
    switch (w->def->kind) {
    case WIDGET_PROGRESS:
        lv_obj_set_style_bg_opa(w->shape, opa, LV_PART_INDICATOR);
        if (w->detail) {
            lv_obj_set_style_text_opa(w->detail, opa, 0);
        }
        break;
    case WIDGET_GAUGE:
        lv_obj_set_style_arc_opa(w->shape, opa, LV_PART_INDICATOR);
        break;
    case WIDGET_GRAPH:
        lv_obj_set_style_line_opa(w->shape, opa, LV_PART_ITEMS);
        break;
    case WIDGET_IMAGE:
        for (uint32_t i = 0; i < lv_obj_get_child_count(w->shape); i++) {
            lv_obj_set_style_image_opa(lv_obj_get_child(w->shape, i), opa, 0);
        }
        break;
    default:
        break;
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
        case WIDGET_IMAGE:
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
        for (int k = 0; k < views[i].count; k++) {
            free_image(&views[i].widgets[k]);
            free(views[i].widgets[k].img_url);
        }
        memset(&views[i], 0, sizeof(views[i]));
    }
}
