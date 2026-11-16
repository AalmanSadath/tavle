#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "fetch.h"
#include "layout.h"
#include "mbedtls/sha256.h"
#include "sources.h"
#include "settings.h"
#include "store.h"
#include "timesync.h"
#include "widgets.h"


static const char *TAG = "ui";

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_28);

extern const char layout_json_start[] asm("_binary_layout_json_start");
extern const char layout_json_end[] asm("_binary_layout_json_end");

#define TOAST_MS 10000

typedef enum {
    MSG_NET, MSG_RELOAD, MSG_LAYOUT, MSG_VALUE, MSG_SRC_STATE, MSG_IMAGE, MSG_NEXT, MSG_SETTINGS,
} msg_kind_t;

#define IMAGE_RETRY_US (20 * 1000000LL)
#define WAIT_US        (3 * 1000000LL) /* longest a switch waits for data */

typedef struct {
    msg_kind_t kind;
    union {
        net_status_t net;
        struct {
            char *json;
            size_t len;
            layout_src_t src;
        } layout;
        struct {
            unsigned gen;
            int index;
            bool is_num;
            double num;
            char path[SRC_PATH_MAX];
            char *text; /* malloc'd, NULL for numbers */
        } value;
        struct {
            unsigned gen;
            int index;
            bool online;
        } src;
        struct {
            unsigned gen;
            int slot;
            char *url;
            png_picture_t *pic;
        } image;
    };
} ui_msg_t;

static QueueHandle_t s_queue;
static lv_display_t *s_disp;
static layout_t s_layout;
static screen_view_t s_views[LAYOUT_MAX_SCREENS]; /* .screen NULL: not live */
static unsigned s_view_gen[LAYOUT_MAX_SCREENS];   /* per build, for picture replies */
static int s_visible;                             /* screen index shown */
static int s_mru[LAYOUT_MAX_SCREENS];             /* screens, most recently viewed first */
static uint16_t s_running_mask;                   /* sources asked to run */
static bool s_src_ready[LAYOUT_MAX_SOURCES];      /* sent data or went offline since start */
static bool s_src_offline[LAYOUT_MAX_SOURCES];
static bool s_keep;                               /* keep other screens running */
static const char *s_keep_from = "board default";
static bool s_pending;                            /* switched, waiting for data */
static int64_t s_pending_since;
static bool s_frozen;                             /* display refresh paused */
static lv_obj_t *s_blank;                         /* shown (frozen) while screens change */
static lv_obj_t *s_info;      /* setup or connecting screen */
static bool s_dashboard_shown;
static uint8_t s_shown_hash[32];
static bool s_have_hash;

/* Last value of every (source, path) a widget reads, so a screen built
 * later shows what a running source already sent. */
typedef struct {
    uint8_t src;
    bool has;
    bool is_num;
    const char *path; /* in s_layout */
    double num;
    char *text;       /* malloc'd, NULL for numbers */
} cached_t;
static cached_t *s_cache;
static int s_cache_n;

static ui_screen_info_t s_info_pub;
static portMUX_TYPE s_info_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- screens and the value cache --------------------------------------- */

/* Index of the source a widget reads, or -1 (clock). */
static int src_of(const layout_widget_t *d)
{
    if (!d->source) {
        return -1;
    }
    for (int i = 0; i < s_layout.source_count; i++) {
        if (strcmp(d->source, s_layout.sources[i].name) == 0) {
            return i;
        }
    }
    return -1;
}

static void cache_free(void)
{
    for (int i = 0; i < s_cache_n; i++) {
        free(s_cache[i].text);
    }
    free(s_cache);
    s_cache = NULL;
    s_cache_n = 0;
}

static cached_t *cache_find(int src, const char *path)
{
    for (int i = 0; i < s_cache_n; i++) {
        if (s_cache[i].src == src && strcmp(s_cache[i].path, path) == 0) {
            return &s_cache[i];
        }
    }
    return NULL;
}

/* One entry per distinct (source, path) any widget of the layout reads. */
static void cache_build(void)
{
    cache_free();
    int max = 0;
    for (int sc = 0; sc < s_layout.screen_count; sc++) {
        for (int i = 0; i < s_layout.screens[sc].widget_count; i++) {
            max += s_layout.screens[sc].widgets[i].path_count;
        }
    }
    s_cache = calloc(max ? max : 1, sizeof(*s_cache));
    if (!s_cache) {
        ESP_LOGW(TAG, "no memory for the value cache");
        return;
    }
    for (int sc = 0; sc < s_layout.screen_count; sc++) {
        for (int i = 0; i < s_layout.screens[sc].widget_count; i++) {
            const layout_widget_t *d = &s_layout.screens[sc].widgets[i];
            int src = src_of(d);
            for (int k = 0; src >= 0 && k < d->path_count; k++) {
                if (!cache_find(src, d->paths[k])) {
                    s_cache[s_cache_n++] = (cached_t){.src = src, .path = d->paths[k]};
                }
            }
        }
    }
}

static void cache_drop_source(int src)
{
    for (int i = 0; i < s_cache_n; i++) {
        if (s_cache[i].src == src) {
            free(s_cache[i].text);
            s_cache[i] = (cached_t){.src = src, .path = s_cache[i].path};
        }
    }
}

static void publish_info(void)
{
    ui_screen_info_t info = {
        .index = s_visible,
        .count = s_layout.screen_count,
        .keep_running = s_keep,
        .keep_from = s_keep_from,
    };
    if (s_layout.screen_count) {
        strlcpy(info.name, s_layout.screens[s_visible].name, sizeof(info.name));
    }
    taskENTER_CRITICAL(&s_info_mux);
    s_info_pub = info;
    taskEXIT_CRITICAL(&s_info_mux);
}

/* "keep other screens running": the setting, else the layout, else the board. */
static void resolve_keep(void)
{
    settings_t s;
    settings_get(&s);
    if (s.keep_running == KEEP_ON || s.keep_running == KEEP_OFF) {
        s_keep = s.keep_running == KEEP_ON;
        s_keep_from = "setting";
    } else if (s_layout.keep_running >= 0) {
        s_keep = s_layout.keep_running;
        s_keep_from = "layout";
    } else {
        s_keep = board_profile.keep_running;
        s_keep_from = "board default";
    }
    memset(&s, 0, sizeof(s));
}

static void mru_touch(int sc)
{
    int at = 0;
    while (at < s_layout.screen_count && s_mru[at] != sc) {
        at++;
    }
    for (int j = at; j > 0; j--) {
        s_mru[j] = s_mru[j - 1];
    }
    s_mru[0] = sc;
}

/* The visible screen, and with keep-running on the most recently viewed
 * others while their sources and pictures fit the board. Returns the
 * sources the live screens read. */
static uint16_t choose_live(bool live[LAYOUT_MAX_SCREENS])
{
    memset(live, 0, LAYOUT_MAX_SCREENS * sizeof(bool));
    const layout_screen_t *v = &s_layout.screens[s_visible];
    live[s_visible] = true;
    uint16_t mask = v->source_mask;
    int images = v->image_count;
    for (int j = 0; s_keep && j < s_layout.screen_count; j++) {
        const int sc = s_mru[j];
        if (sc == s_visible) {
            continue;
        }
        const layout_screen_t *o = &s_layout.screens[sc];
        uint16_t m = mask | o->source_mask;
        if (__builtin_popcount(m) > board_profile.max_sources ||
            images + o->image_count > board_profile.max_images) {
            break; /* this one and every older screen stop */
        }
        live[sc] = true;
        mask = m;
        images += o->image_count;
    }
    return mask;
}

/* With the LVGL lock held. The panel keeps its last image meanwhile. */
static void begin_wait_locked(void)
{
    if (!s_frozen) {
        display_freeze(true);
        display_draw_mark();
        s_frozen = true;
    }
}

static void end_wait_locked(void)
{
    if (s_frozen) {
        display_freeze(false);
        s_frozen = false;
    }
    s_pending = false;
}

static void apply_value(widget_view_t *w, int slot, int k, bool is_num, double num,
                        const char *text, int64_t now);

/* Fills a freshly built screen from the cache. LVGL lock held. */
static void replay(int sc)
{
    int64_t now = esp_timer_get_time();
    screen_view_t *v = &s_views[sc];
    for (int i = 0; i < v->count; i++) {
        widget_view_t *w = &v->widgets[i];
        int src = src_of(w->def);
        if (src < 0) {
            continue;
        }
        for (int k = 0; k < w->def->path_count; k++) {
            cached_t *c = cache_find(src, w->def->paths[k]);
            if (c && c->has) {
                apply_value(w, sc * LAYOUT_MAX_WIDGETS + i, k, c->is_num, c->num, c->text, now);
            }
        }
        widget_set_stale(w, s_src_offline[src]);
    }
}

/* Builds the live screens, deletes the others and runs exactly the sources
 * the live ones read. A visible screen that has to be built is shown only
 * once its sources have sent data, or after WAIT_US. */
static void update_live(void)
{
    bool live[LAYOUT_MAX_SCREENS];
    uint16_t want = choose_live(live);
    bool shown_ready = s_views[s_visible].screen != NULL;

    lvgl_port_lock(0);
    if (s_dashboard_shown) {
        if (shown_ready) {
            lv_screen_load(s_views[s_visible].screen);
        } else {
            begin_wait_locked();
            lv_screen_load(s_blank);
        }
    }
    for (int sc = 0; sc < LAYOUT_MAX_SCREENS; sc++) {
        if (!live[sc] && s_views[sc].screen) {
            widgets_destroy(&s_views[sc], 1);
        }
    }
    lvgl_port_unlock();

    sources_apply(want);
    for (int i = 0; i < s_layout.source_count; i++) {
        uint16_t bit = 1u << i;
        if ((s_running_mask & bit) && !(want & bit)) {
            cache_drop_source(i);
        } else if (!(s_running_mask & bit) && (want & bit)) {
            s_src_ready[i] = false;
            s_src_offline[i] = false;
        }
    }
    s_running_mask = want;

    lvgl_port_lock(0);
    for (int sc = 0; sc < s_layout.screen_count; sc++) {
        if (live[sc] && !s_views[sc].screen) {
            widgets_build_screen(&s_layout.screens[sc], &s_views[sc]);
            s_view_gen[sc]++;
            replay(sc);
        }
    }
    if (s_dashboard_shown && !shown_ready) {
        lv_screen_load(s_views[s_visible].screen);
        s_pending = true;
        s_pending_since = esp_timer_get_time();
    }
    lvgl_port_unlock();
    publish_info();
    ESP_LOGI(TAG, "screen %d of %d '%s', sources running 0x%x, free heap %u, largest block %u",
             s_visible + 1, s_layout.screen_count, s_layout.screens[s_visible].name, want,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

/* Shows the pending screen once every source it reads has sent data or
 * reported offline, or WAIT_US after the switch. */
static void check_pending(void)
{
    if (!s_pending) {
        return;
    }
    int64_t waited = esp_timer_get_time() - s_pending_since;
    bool ready = true;
    uint16_t mask = s_layout.screens[s_visible].source_mask;
    for (int i = 0; i < s_layout.source_count; i++) {
        if ((mask & (1u << i)) && sources_running(i) && !s_src_ready[i]) {
            ready = false;
        }
    }
    if (!ready && waited < WAIT_US) {
        return;
    }
    lvgl_port_lock(0);
    end_wait_locked();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "screen '%s' shown after %d ms%s", s_layout.screens[s_visible].name,
             (int)(waited / 1000), ready ? "" : ", some values still missing");
}

static void next_screen(void)
{
    if (!s_dashboard_shown || s_layout.screen_count < 2) {
        return;
    }
    s_visible = (s_visible + 1) % s_layout.screen_count;
    mru_touch(s_visible);
    update_live();
    check_pending();
}

/* ---- layout ------------------------------------------------------------ */

/* Parses json and, if it holds a usable screen that fits the board,
 * replaces the dashboard with it; a bad layout keeps what is on screen. A
 * fetched layout that differs from the one shown is also saved to flash.
 * Takes ownership of json. */
static bool apply_layout(char *json, size_t len, layout_src_t src)
{
    uint8_t hash[32];
    mbedtls_sha256((const unsigned char *)json, len, hash, 0);
    if (s_have_hash && memcmp(hash, s_shown_hash, sizeof(hash)) == 0) {
        free(json);
        if (src == LAYOUT_SRC_FETCHED) {
            ESP_LOGI(TAG, "fetched layout unchanged");
            fetch_report_shown(LAYOUT_SRC_FETCHED, len);
            fetch_report_result(true, NULL);
        }
        return true;
    }

    /* Static: a layout is several KB, too much for the stack. */
    static layout_t fresh;
    if (!layout_parse(json, len, &fresh)) {
        free(json);
        if (src == LAYOUT_SRC_FETCHED) {
            char msg[112];
            snprintf(msg, sizeof(msg), "invalid layout: %s", layout_last_error());
            fetch_report_result(false, msg);
        }
        ESP_LOGE(TAG, "%s layout rejected: %s", layout_src_name(src), layout_last_error());
        return false;
    }

    /* The new layout is valid: free the old one before building, so the new
     * screens reuse its memory instead of leaving holes that split the heap
     * (a picture needs one 32 KB block). */
    sources_stop_all();
    lvgl_port_lock(0);
    if (s_dashboard_shown) {
        begin_wait_locked();
        lv_screen_load(s_blank);
    }
    widgets_destroy(s_views, LAYOUT_MAX_SCREENS);
    layout_free(&s_layout);
    lvgl_port_unlock();

    s_layout = fresh; /* the cJSON tree moves with it; widget defs are taken from here */
    memset(&fresh, 0, sizeof(fresh));
    if (s_visible >= s_layout.screen_count) {
        s_visible = 0;
    }
    for (int i = 0; i < s_layout.screen_count; i++) {
        s_mru[i] = i;
    }
    mru_touch(s_visible);
    cache_build();
    memset(s_src_ready, 0, sizeof(s_src_ready));
    memset(s_src_offline, 0, sizeof(s_src_offline));
    s_running_mask = 0;
    sources_load(&s_layout);
    resolve_keep();
    update_live();

    memcpy(s_shown_hash, hash, sizeof(hash));
    s_have_hash = true;
    if (src == LAYOUT_SRC_FETCHED) {
        store_save(json, len);
        fetch_report_result(true, NULL);
    }
    fetch_report_shown(src, len);
    free(json);
    ESP_LOGI(TAG, "%s layout shown (%u bytes), keep screens running %s (%s)",
             layout_src_name(src), (unsigned)len, s_keep ? "on" : "off", s_keep_from);
    check_pending();
    return true;
}

static char *builtin_copy(size_t *len)
{
    size_t n = layout_json_end - layout_json_start;
    while (n && layout_json_start[n - 1] == '\0') {
        n--; /* EMBED_TXTFILES appends a terminator */
    }
    char *buf = malloc(n + 1);
    if (buf) {
        memcpy(buf, layout_json_start, n);
        buf[n] = '\0';
        *len = n;
    }
    return buf;
}

/* Saved layout if there is a valid one, else the one in the firmware. */
static void show_offline_layout(void)
{
    size_t len;
    char *json = store_load(&len);
    if (json && apply_layout(json, len, LAYOUT_SRC_SAVED)) {
        return;
    }
    json = builtin_copy(&len);
    if (json) {
        apply_layout(json, len, LAYOUT_SRC_BUILTIN);
    }
}

/* ---- info screens ------------------------------------------------------ */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *info_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 16, 0);
    lv_obj_set_style_pad_row(scr, 12, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return scr;
}

static void show_info(lv_obj_t *scr)
{
    end_wait_locked(); /* a pending switch would keep the display frozen */
    lv_screen_load(scr);
    if (s_info) {
        lv_obj_delete(s_info);
    }
    s_info = scr;
    s_dashboard_shown = false;
}

static void show_setup(const net_status_t *n)
{
    char buf[96];
    lvgl_port_lock(0);
    lv_obj_t *scr = info_screen();
    label(scr, &lv_font_montserrat_28, 0xffffff, "tavle setup");

    if (n->state == NET_FALLBACK) {
        snprintf(buf, sizeof(buf), "can't reach %s", n->ssid);
        label(scr, &lv_font_montserrat_14, 0xff9f0a, buf);
    }

    snprintf(buf, sizeof(buf), "WIFI:T:WPA;S:%s;P:%s;;", n->ap_ssid, n->ap_pass);
    lv_obj_t *qr = lv_qrcode_create(scr);
    int short_side = display_width() < display_height() ? display_width() : display_height();
    lv_qrcode_set_size(qr, short_side / 2); /* 160 on 320 x 480 */
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 8, 0);
    lv_qrcode_update(qr, buf, strlen(buf));

    label(scr, &lv_font_montserrat_14, 0x8e8e93, "join the wi-fi");
    label(scr, &lv_font_montserrat_20, 0xffffff, n->ap_ssid);
    label(scr, &lv_font_montserrat_14, 0x8e8e93, "password");
    label(scr, &lv_font_montserrat_20, 0xffffff, n->ap_pass);
    label(scr, &lv_font_montserrat_14, 0x8e8e93, "then open");
    label(scr, &lv_font_montserrat_20, 0x0a84ff, "http://192.168.4.1");
    show_info(scr);
    lvgl_port_unlock();
}

static void show_connecting(const net_status_t *n)
{
    char buf[64];
    lvgl_port_lock(0);
    lv_obj_t *scr = info_screen();
    label(scr, &lv_font_montserrat_28, 0xffffff, "tavle");
    snprintf(buf, sizeof(buf), "connecting to %s", n->ssid);
    label(scr, &lv_font_montserrat_14, 0x8e8e93, buf);
    show_info(scr);
    lvgl_port_unlock();
}

static void show_dashboard(const net_status_t *n)
{
    char buf[64];
    lvgl_port_lock(0);
    lv_screen_load(s_views[s_visible].screen);
    s_dashboard_shown = true;
    if (s_info) {
        lv_obj_delete(s_info);
        s_info = NULL;
    }

    /* Address toast on the top layer, gone after TOAST_MS. */
    lv_obj_t *toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(toast, display_width() * 3 / 4, LV_SIZE_CONTENT);
    lv_obj_align(toast, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_border_width(toast, 0, 0);
    lv_obj_set_style_radius(toast, 12, 0);
    lv_obj_set_style_pad_all(toast, 12, 0);
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(toast, LV_FLEX_FLOW_COLUMN);
    snprintf(buf, sizeof(buf), "%s.local", n->hostname[0] ? n->hostname : "tavle");
    label(toast, &lv_font_montserrat_20, 0xffffff, buf);
    snprintf(buf, sizeof(buf), "%s  on  %s", n->ip, n->ssid);
    label(toast, &lv_font_montserrat_14, 0x8e8e93, buf);
    lv_obj_delete_delayed(toast, TOAST_MS);
    lvgl_port_unlock();
}

static void on_net(const net_status_t *n)
{
    switch (n->state) {
    case NET_SETUP:
    case NET_FALLBACK:
        show_setup(n);
        break;
    case NET_CONNECTING:
        /* A short drop keeps the dashboard; the hotspot screen follows after
         * the fallback delay if it does not come back. */
        if (!s_dashboard_shown) {
            show_connecting(n);
        }
        break;
    case NET_CONNECTED:
        show_dashboard(n);
        break;
    }
}

/* ---- benchmark --------------------------------------------------------- */

#if CONFIG_TAVLE_BENCH
static int64_t timed_refresh(void)
{
    int64_t t0 = esp_timer_get_time();
    lv_refr_now(s_disp);
    display_wait_idle();
    return esp_timer_get_time() - t0;
}

static void bench(void)
{
    screen_view_t *view = &s_views[0];
    const int full_runs = 20, update_runs = 100;
    int64_t total = 0, worst = 0;

    lvgl_port_lock(0);
    lv_screen_load(view->screen);
    for (int i = 0; i < full_runs; i++) {
        lv_obj_invalidate(view->screen);
        int64_t us = timed_refresh();
        total += us;
        worst = us > worst ? us : worst;
    }
    ESP_LOGI(TAG, "bench full screen @ %d MHz: avg %.1f ms (%.1f fps), worst %.1f ms",
             CONFIG_TAVLE_LCD_SPI_MHZ, total / 1000.0 / full_runs, 1e6 * full_runs / total,
             worst / 1000.0);

    total = worst = 0;
    for (int i = 0; i < update_runs; i++) {
        widgets_bench_step(view, i);
        int64_t us = timed_refresh();
        total += us;
        worst = us > worst ? us : worst;
    }
    widgets_reset(view);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "bench all widgets changing @ %d MHz: avg %.1f ms (%.1f fps), worst %.1f ms",
             CONFIG_TAVLE_LCD_SPI_MHZ, total / 1000.0 / update_runs, 1e6 * update_runs / total,
             worst / 1000.0);
}
#endif

/* ---- live data --------------------------------------------------------- */

static bool bound_to(const widget_view_t *w, int index)
{
    return w->def->source && index < s_layout.source_count &&
           strcmp(w->def->source, s_layout.sources[index].name) == 0;
}

/* Which of a widget's paths this is, or -1. */
static int path_index(const widget_view_t *w, const char *path)
{
    for (int k = 0; k < w->def->path_count; k++) {
        if (strcmp(w->def->paths[k], path) == 0) {
            return k;
        }
    }
    return -1;
}

static void request_image(widget_view_t *w, int slot)
{
    int mw, mh;
    uint32_t bg;
    widget_image_box(w, &mw, &mh, &bg);
    w->img_retry_us = 0;
    fetch_image(s_view_gen[slot / LAYOUT_MAX_WIDGETS], slot, w->img_url, mw, mh, bg,
                w->def->crop);
}

/* An image widget's value is the address of a PNG; "" or "--" means none. */
static void show_image(widget_view_t *w, int slot, const char *url)
{
    bool none = !url[0] || strcmp(url, "--") == 0;
    if (none) {
        free(w->img_url);
        w->img_url = NULL;
        w->img_retry_us = 0;
        widget_set_image(w, NULL);
        return;
    }
    if (w->img_url && strcmp(w->img_url, url) == 0) {
        return; /* shown, or on its way */
    }
    char *copy = strdup(url);
    if (!copy) {
        return;
    }
    free(w->img_url);
    w->img_url = copy;
    request_image(w, slot);
}

static void on_image(ui_msg_t *m)
{
    int sc = m->image.slot / LAYOUT_MAX_WIDGETS, i = m->image.slot % LAYOUT_MAX_WIDGETS;
    widget_view_t *w = NULL;
    if (sc < LAYOUT_MAX_SCREENS && m->image.gen == s_view_gen[sc] && s_views[sc].screen &&
        i < s_views[sc].count) {
        w = &s_views[sc].widgets[i];
    }
    /* only the picture the widget still wants */
    if (!w || w->def->kind != WIDGET_IMAGE || !w->img_url || !m->image.url ||
        strcmp(w->img_url, m->image.url) != 0) {
        png_picture_free(m->image.pic);
    } else if (!m->image.pic) {
        w->img_retry_us = esp_timer_get_time() + IMAGE_RETRY_US;
    } else {
        lvgl_port_lock(0);
        widget_set_image(w, m->image.pic);
        lvgl_port_unlock();
    }
    free(m->image.url);
}

/* Shows path k of widget w. text is NULL for numbers. LVGL lock held. */
static void apply_value(widget_view_t *w, int slot, int k, bool is_num, double num,
                        const char *text, int64_t now)
{
    widget_set_stale(w, false);
    if (w->def->kind == WIDGET_IMAGE) {
        show_image(w, slot, is_num ? "" : text);
        return;
    }
    if (w->def->kind == WIDGET_PROGRESS && k == 1) {
        char buf[24];
        if (is_num) {
            snprintf(buf, sizeof(buf), "%g", num);
        }
        widget_set_detail(w, is_num ? buf : text);
        return;
    }
    if (!is_num) {
        widget_set_text(w, text);
        return;
    }
    if (w->def->kind == WIDGET_GRAPH) {
        /* sampled on the tick; the first value of each line shows at once */
        if (!w->has_last[k]) {
            widget_graph_push(w, k, num);
            if (!w->next_sample_us) {
                w->next_sample_us = now + w->def->period_s * 1000000LL;
            }
        }
    } else {
        widget_set_number(w, num);
    }
    w->last[k] = num;
    w->has_last[k] = true;
}

static void on_value(ui_msg_t *m)
{
    const int src = m->value.index;
    if (src < 0 || src >= s_layout.source_count || m->value.gen != sources_generation(src)) {
        return; /* from a driver that has stopped since */
    }
    cached_t *c = cache_find(src, m->value.path);
    if (!c) {
        return; /* no widget reads it */
    }
    free(c->text);
    *c = (cached_t){.src = src, .path = c->path, .has = true, .is_num = m->value.is_num,
                    .num = m->value.num, .text = m->value.text};
    m->value.text = NULL; /* the cache owns it now */
    s_src_ready[src] = true;
    s_src_offline[src] = false;

    int64_t now = esp_timer_get_time();
    lvgl_port_lock(0);
    for (int sc = 0; sc < s_layout.screen_count; sc++) {
        for (int i = 0; i < s_views[sc].count; i++) {
            widget_view_t *w = &s_views[sc].widgets[i];
            int k;
            if (bound_to(w, src) && (k = path_index(w, c->path)) >= 0) {
                apply_value(w, sc * LAYOUT_MAX_WIDGETS + i, k, c->is_num, c->num, c->text, now);
            }
        }
    }
    lvgl_port_unlock();
}

static void on_source_state(const ui_msg_t *m)
{
    const int src = m->src.index;
    if (src < 0 || src >= s_layout.source_count || m->src.gen != sources_generation(src)) {
        return;
    }
    s_src_offline[src] = !m->src.online;
    if (!m->src.online) {
        s_src_ready[src] = true; /* nothing to wait for */
    }
    lvgl_port_lock(0);
    for (int sc = 0; sc < s_layout.screen_count; sc++) {
        for (int i = 0; i < s_views[sc].count; i++) {
            widget_view_t *w = &s_views[sc].widgets[i];
            if (bound_to(w, src)) {
                widget_set_stale(w, !m->src.online);
            }
        }
    }
    lvgl_port_unlock();
}

/* Once a second: clocks and graph samples. */
static void tick(void)
{
    timesync_apply_tz();
    struct tm tm_now;
    const struct tm *now_tm = NULL;
    if (timesync_valid()) {
        time_t t = time(NULL);
        localtime_r(&t, &tm_now);
        now_tm = &tm_now;
    }
    int64_t now = esp_timer_get_time();

    lvgl_port_lock(0);
    for (int sc = 0; sc < s_layout.screen_count; sc++) {
        if (!s_views[sc].screen) {
            continue;
        }
        widgets_update_clocks(&s_views[sc], now_tm);
        for (int i = 0; i < s_views[sc].count; i++) {
            widget_view_t *w = &s_views[sc].widgets[i];
            if (w->def->kind == WIDGET_IMAGE && w->img_retry_us && now >= w->img_retry_us) {
                request_image(w, sc * LAYOUT_MAX_WIDGETS + i);
            }
            if (w->def->kind == WIDGET_GRAPH && w->next_sample_us && !w->stale &&
                now >= w->next_sample_us) {
                for (int k = 0; k < w->def->path_count; k++) {
                    if (w->has_last[k]) {
                        widget_graph_push(w, k, w->last[k]);
                    }
                }
                w->next_sample_us = now + w->def->period_s * 1000000LL;
            }
        }
    }
    lvgl_port_unlock();
}

/* ---- task -------------------------------------------------------------- */

static void ui_task(void *arg)
{
    /* LVGL renders on the stack of whoever holds the lock, so all UI work
     * happens here rather than on small event or HTTP stacks. */
    show_offline_layout();
#if CONFIG_TAVLE_BENCH
    if (s_layout.screen_count) {
        bench();
    }
#endif

    ui_msg_t msg;
    int64_t next_tick = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now >= next_tick) {
            tick();
            next_tick = now + 1000000;
        }
        int64_t until = next_tick;
        if (s_pending && s_pending_since + WAIT_US < until) {
            until = s_pending_since + WAIT_US;
        }
        TickType_t wait = pdMS_TO_TICKS((until > now ? until - now : 0) / 1000 + 1);
        if (xQueueReceive(s_queue, &msg, wait)) {
            if (msg.kind == MSG_VALUE) {
                on_value(&msg);
                free(msg.value.text);
            } else if (msg.kind == MSG_IMAGE) {
                on_image(&msg);
            } else if (msg.kind == MSG_SRC_STATE) {
                on_source_state(&msg);
            } else if (msg.kind == MSG_NET) {
                on_net(&msg.net);
            } else if (msg.kind == MSG_RELOAD) {
                s_have_hash = false; /* rebuild even if unchanged */
                show_offline_layout();
            } else if (msg.kind == MSG_LAYOUT) {
                apply_layout(msg.layout.json, msg.layout.len, msg.layout.src);
            } else if (msg.kind == MSG_NEXT) {
                next_screen();
            } else if (msg.kind == MSG_SETTINGS && s_layout.screen_count) {
                bool before = s_keep;
                resolve_keep();
                if (before != s_keep) {
                    ESP_LOGI(TAG, "keep screens running %s (%s)", s_keep ? "on" : "off",
                             s_keep_from);
                    update_live();
                }
                publish_info();
            }
            memset(&msg, 0, sizeof(msg)); /* hotspot password */
        }
        check_pending();
    }
}

esp_err_t ui_start(lv_display_t *disp)
{
    s_disp = disp;
    s_queue = xQueueCreate(12, sizeof(ui_msg_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }

    lvgl_port_lock(0);
    widgets_init(disp);
    s_blank = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_blank, lv_color_black(), 0);
    lv_obj_t *scr = info_screen();
    label(scr, &lv_font_montserrat_28, 0xffffff, "tavle");
    show_info(scr);
    lvgl_port_unlock();

    return xTaskCreate(ui_task, "ui", 8192, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void ui_next_screen(void)
{
    ui_msg_t msg = {.kind = MSG_NEXT};
    xQueueSend(s_queue, &msg, 0);
}

void ui_settings_changed(void)
{
    ui_msg_t msg = {.kind = MSG_SETTINGS};
    xQueueSend(s_queue, &msg, pdMS_TO_TICKS(100));
}

void ui_get_screen_info(ui_screen_info_t *out)
{
    taskENTER_CRITICAL(&s_info_mux);
    *out = s_info_pub;
    taskEXIT_CRITICAL(&s_info_mux);
}

void ui_net_status(const net_status_t *status)
{
    ui_msg_t msg = {.kind = MSG_NET, .net = *status};
    xQueueSend(s_queue, &msg, pdMS_TO_TICKS(100));
}

void ui_set_layout(char *json, size_t len, layout_src_t src)
{
    ui_msg_t msg = {.kind = MSG_LAYOUT, .layout = {.json = json, .len = len, .src = src}};
    if (xQueueSend(s_queue, &msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(json);
        fetch_report_result(false, "display busy, try again");
    }
}

/* Drivers must never block on the UI: a full queue drops the update, and the
 * next one replaces it. */
void ui_source_value(unsigned gen, int index, const char *path, bool is_num, double num,
                     const char *text)
{
    ui_msg_t msg = {.kind = MSG_VALUE,
                    .value = {.gen = gen, .index = index, .is_num = is_num, .num = num}};
    strlcpy(msg.value.path, path, sizeof(msg.value.path));
    if (text) {
        msg.value.text = strndup(text, SRC_TEXT_MAX);
        if (!msg.value.text) {
            return;
        }
    }
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        free(msg.value.text);
    }
}

void ui_image_ready(unsigned gen, int slot, char *url, png_picture_t *pic)
{
    ui_msg_t msg = {.kind = MSG_IMAGE, .image = {.gen = gen, .slot = slot, .url = url, .pic = pic}};
    if (xQueueSend(s_queue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
        free(url);
        png_picture_free(pic);
    }
}

void ui_source_state(unsigned gen, int index, bool online)
{
    ui_msg_t msg = {.kind = MSG_SRC_STATE, .src = {.gen = gen, .index = index, .online = online}};
    /* state changes are rare and matter more than one value: wait briefly */
    xQueueSend(s_queue, &msg, pdMS_TO_TICKS(50));
}

void ui_request_reload(void)
{
    ui_msg_t msg = {.kind = MSG_RELOAD};
    xQueueSend(s_queue, &msg, pdMS_TO_TICKS(100));
}
