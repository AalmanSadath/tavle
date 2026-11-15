#include "layout.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "layout";
static char s_error[64];

const char *layout_last_error(void)
{
    return s_error;
}

static const char *const KIND_NAMES[] = {
    [WIDGET_TEXT] = "text",
    [WIDGET_VALUE] = "value",
    [WIDGET_PROGRESS] = "progress",
    [WIDGET_GAUGE] = "gauge",
    [WIDGET_GRAPH] = "graph",
    [WIDGET_ICON] = "icon",
    [WIDGET_CLOCK] = "clock",
};

const char *widget_kind_name(widget_kind_t kind)
{
    return KIND_NAMES[kind];
}

static bool kind_from_name(const char *s, widget_kind_t *out)
{
    for (size_t i = 0; i < sizeof(KIND_NAMES) / sizeof(KIND_NAMES[0]); i++) {
        if (strcmp(s, KIND_NAMES[i]) == 0) {
            *out = (widget_kind_t)i;
            return true;
        }
    }
    return false;
}

static bool source_type_from_name(const char *s, source_type_t *out)
{
    if (strcmp(s, "http-json") == 0) {
        *out = SOURCE_HTTP_JSON;
    } else if (strcmp(s, "moonraker") == 0) {
        *out = SOURCE_MOONRAKER;
    } else if (strcmp(s, "mqtt") == 0) {
        *out = SOURCE_MQTT;
    } else {
        return false;
    }
    return true;
}

static const char *get_str(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int get_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : def;
}

static double get_num(const cJSON *obj, const char *key, double def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : def;
}

/* The layout comes from a URL, so a number format is passed to snprintf
 * only when it holds exactly one floating-point conversion: flags, width,
 * precision and one of f F e E g G. Literal text and %% are allowed. */
static bool number_format_ok(const char *fmt)
{
    int conversions = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            continue;
        }
        p++;
        if (*p == '%') {
            continue;
        }
        while (*p && strchr("-+ #0", *p)) {
            p++;
        }
        while (isdigit((unsigned char)*p)) {
            p++;
        }
        if (*p == '.') {
            p++;
            while (isdigit((unsigned char)*p)) {
                p++;
            }
        }
        if (!*p || !strchr("fFeEgG", *p)) {
            return false;
        }
        conversions++;
    }
    return conversions == 1 && strlen(fmt) < 32;
}

static bool source_exists(const layout_t *l, const char *name)
{
    for (int i = 0; i < l->source_count; i++) {
        if (strcmp(l->sources[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static void parse_sources(const cJSON *sources, layout_t *l)
{
    const cJSON *src;
    cJSON_ArrayForEach(src, sources) {
        if (l->source_count == LAYOUT_MAX_SOURCES) {
            ESP_LOGW(TAG, "more than %d sources, rest ignored", LAYOUT_MAX_SOURCES);
            break;
        }
        const char *type = get_str(src, "type");
        source_type_t t;
        if (!cJSON_IsObject(src) || !type || !source_type_from_name(type, &t)) {
            ESP_LOGW(TAG, "source '%s': unknown or missing type, skipped", src->string);
            continue;
        }
        l->sources[l->source_count++] = (layout_source_t){
            .name = src->string,
            .type = t,
            .config = src,
        };
    }
}

static bool parse_widget(const cJSON *w, const layout_t *l, uint16_t *used,
                         layout_widget_t *out, const char *screen, int index)
{
    const char *type = get_str(w, "type");
    if (!type || !kind_from_name(type, &out->kind)) {
        ESP_LOGW(TAG, "%s[%d]: unknown type '%s', skipped", screen, index, type ? type : "");
        return false;
    }

    out->col = get_int(w, "col", -1);
    out->row = get_int(w, "row", -1);
    out->colspan = get_int(w, "colspan", 1);
    out->rowspan = get_int(w, "rowspan", 1);
    if (out->col < 0 || out->row < 0 || out->colspan < 1 || out->rowspan < 1 ||
        out->col + out->colspan > LAYOUT_COLS || out->row + out->rowspan > LAYOUT_ROWS) {
        ESP_LOGW(TAG, "%s[%d]: cell col=%d row=%d span=%dx%d outside the %dx%d grid, skipped",
                 screen, index, out->col, out->row, out->colspan, out->rowspan,
                 LAYOUT_COLS, LAYOUT_ROWS);
        return false;
    }

    uint16_t mask = 0;
    for (int r = out->row; r < out->row + out->rowspan; r++) {
        for (int c = out->col; c < out->col + out->colspan; c++) {
            mask |= 1u << (r * LAYOUT_COLS + c);
        }
    }
    if (mask & *used) {
        ESP_LOGW(TAG, "%s[%d]: overlaps an earlier widget, skipped", screen, index);
        return false;
    }

    out->source = get_str(w, "source");
    out->path = get_str(w, "path");
    out->path_count = 0;
    if (out->path) {
        out->paths[0] = out->path;
        out->path_count = 1;
    }
    if (out->kind != WIDGET_CLOCK) {
        if (!out->source || !out->path) {
            ESP_LOGW(TAG, "%s[%d]: %s needs source and path, skipped", screen, index, type);
            return false;
        }
        if (!source_exists(l, out->source)) {
            ESP_LOGW(TAG, "%s[%d]: source '%s' not defined, skipped", screen, index, out->source);
            return false;
        }
    }

    out->format = get_str(w, "format");
    if (out->kind == WIDGET_CLOCK) {
        if (!out->format || strlen(out->format) > 16) {
            out->format = "%H:%M";
        }
    } else if (out->format && !number_format_ok(out->format)) {
        ESP_LOGW(TAG, "%s[%d]: format '%s' rejected, using default", screen, index, out->format);
        out->format = NULL;
    }

    out->unit = get_str(w, "unit");
    out->label = get_str(w, "label");
    out->min = get_num(w, "min", 0);
    out->max = get_num(w, "max", out->kind == WIDGET_PROGRESS ? 1 : 100);
    if (out->max <= out->min) {
        out->max = out->min + 1;
    }
    out->period_s = get_int(w, "period", 10);
    if (out->period_s < 1 || out->period_s > 3600) {
        out->period_s = 10;
    }

    *used |= mask;
    return true;
}

bool layout_parse(const char *json, size_t len, layout_t *out)
{
    memset(out, 0, sizeof(*out));
    s_error[0] = '\0';
    out->root = cJSON_ParseWithLength(json, len);
    if (!out->root) {
        const char *at = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "JSON parse error near: %.32s", at ? at : "");
        snprintf(s_error, sizeof(s_error), "not valid JSON (at byte %d)",
                 at ? (int)(at - json) : -1);
        return false;
    }

    parse_sources(cJSON_GetObjectItemCaseSensitive(out->root, "sources"), out);

    const cJSON *screens = cJSON_GetObjectItemCaseSensitive(out->root, "screens");
    const cJSON *s;
    cJSON_ArrayForEach(s, screens) {
        if (out->screen_count == LAYOUT_MAX_SCREENS) {
            ESP_LOGW(TAG, "more than %d screens, rest ignored", LAYOUT_MAX_SCREENS);
            break;
        }
        layout_screen_t *scr = &out->screens[out->screen_count];
        scr->name = get_str(s, "name");
        if (!scr->name) {
            scr->name = "screen";
        }

        uint16_t used = 0;
        int index = 0;
        const cJSON *w;
        cJSON_ArrayForEach(w, cJSON_GetObjectItemCaseSensitive(s, "widgets")) {
            if (scr->widget_count < LAYOUT_MAX_WIDGETS &&
                parse_widget(w, out, &used, &scr->widgets[scr->widget_count], scr->name, index)) {
                scr->widget_count++;
            }
            index++;
        }

        if (scr->widget_count == 0) {
            /* An empty screen would show nothing; it does not count. */
            ESP_LOGW(TAG, "screen '%s': no valid widgets, skipped", scr->name);
            memset(scr, 0, sizeof(*scr));
            continue;
        }
        ESP_LOGI(TAG, "screen '%s': %d widgets", scr->name, scr->widget_count);
        out->screen_count++;
    }

    ESP_LOGI(TAG, "%d sources, %d screens", out->source_count, out->screen_count);
    if (out->screen_count == 0) {
        snprintf(s_error, sizeof(s_error), "no usable screen");
        layout_free(out);
        return false;
    }
    return true;
}

void layout_free(layout_t *layout)
{
    cJSON_Delete(layout->root);
    memset(layout, 0, sizeof(*layout));
}
