#include "sources.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "src_moonraker.h"
#include "ui.h"

static const char *TAG = "sources";

static const source_driver_t *const DRIVERS[] = {
    &src_moonraker_driver,
    /* http-json and mqtt come later */
};

typedef struct {
    const source_driver_t *drv;
    void *ctx;
} running_t;

static running_t s_running[LAYOUT_MAX_SOURCES];
static int s_count;
static source_status_t s_status[LAYOUT_MAX_SOURCES];
static SemaphoreHandle_t s_lock;
static volatile unsigned s_gen;

const char *src_state_name(src_state_t state)
{
    static const char *const names[] = {"starting", "online", "offline", "not built"};
    return names[state];
}

static const source_driver_t *find_driver(source_type_t type)
{
    static const char *const names[] = {
        [SOURCE_HTTP_JSON] = "http-json",
        [SOURCE_MOONRAKER] = "moonraker",
        [SOURCE_MQTT] = "mqtt",
    };
    for (size_t i = 0; i < sizeof(DRIVERS) / sizeof(DRIVERS[0]); i++) {
        if (strcmp(DRIVERS[i]->type, names[type]) == 0) {
            return DRIVERS[i];
        }
    }
    return NULL;
}

static void set_status(int index, src_state_t state, const char *detail)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (index < s_count) {
        s_status[index].state = state;
        strlcpy(s_status[index].detail, detail ? detail : "", sizeof(s_status[index].detail));
    }
    xSemaphoreGive(s_lock);
}

void sources_stop(void)
{
    for (int i = 0; i < s_count; i++) {
        if (s_running[i].drv && s_running[i].ctx) {
            s_running[i].drv->stop(s_running[i].ctx); /* no reports after this */
        }
        s_running[i] = (running_t){0};
    }
    s_gen++;
}

/* Collects the distinct paths widgets read from one source. */
static int collect_paths(const layout_t *l, const char *name, const char **out, int max)
{
    int n = 0;
    for (int s = 0; s < l->screen_count; s++) {
        for (int w = 0; w < l->screens[s].widget_count; w++) {
            const layout_widget_t *d = &l->screens[s].widgets[w];
            if (!d->source || strcmp(d->source, name) != 0) {
                continue;
            }
            for (int p = 0; p < d->path_count; p++) {
                bool dup = false;
                for (int i = 0; i < n; i++) {
                    dup |= strcmp(out[i], d->paths[p]) == 0;
                }
                if (!dup && n < max && strlen(d->paths[p]) < SRC_PATH_MAX) {
                    out[n++] = d->paths[p];
                }
            }
        }
    }
    return n;
}

void sources_start(const layout_t *layout)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    sources_stop();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = layout->source_count;
    memset(s_status, 0, sizeof(s_status));
    for (int i = 0; i < s_count; i++) {
        strlcpy(s_status[i].name, layout->sources[i].name, sizeof(s_status[i].name));
        const char *t = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(layout->sources[i].config, "type"));
        strlcpy(s_status[i].type, t ? t : "", sizeof(s_status[i].type));
    }
    xSemaphoreGive(s_lock);

    for (int i = 0; i < s_count; i++) {
        const layout_source_t *src = &layout->sources[i];
        const char *paths[LAYOUT_MAX_SCREENS * LAYOUT_MAX_WIDGETS];
        int n = collect_paths(layout, src->name, paths, sizeof(paths) / sizeof(paths[0]));

        const source_driver_t *drv = find_driver(src->type);
        if (!drv) {
            ESP_LOGW(TAG, "source '%s': type not built yet", src->name);
            set_status(i, SRC_NOT_BUILT, "this source type is not built yet");
            continue;
        }
        if (n == 0) {
            set_status(i, SRC_OFFLINE, "no widget uses this source");
            continue;
        }
        set_status(i, SRC_STARTING, "");
        s_running[i].drv = drv;
        s_running[i].ctx = drv->start(i, src->config, paths, n);
        if (!s_running[i].ctx) {
            s_running[i].drv = NULL;
            if (s_status[i].state == SRC_STARTING) {
                set_status(i, SRC_OFFLINE, "could not start");
            }
        }
    }
}

unsigned sources_generation(void)
{
    return s_gen;
}

void sources_report_number(int index, const char *path, double v)
{
    ui_source_value(s_gen, index, path, true, v, NULL);
}

void sources_report_text(int index, const char *path, const char *s)
{
    ui_source_value(s_gen, index, path, false, 0, s);
}

void sources_report_state(int index, src_state_t state, const char *detail)
{
    set_status(index, state, detail);
    ui_source_state(s_gen, index, state == SRC_ONLINE);
}

int sources_get_status(source_status_t *out, int max)
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_count < max ? s_count : max;
    memcpy(out, s_status, n * sizeof(*out));
    xSemaphoreGive(s_lock);
    return n;
}
