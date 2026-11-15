#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#include "store.h"
#include "widgets.h"

#if CONFIG_TAVLE_BENCH
#include "display.h"
#endif

static const char *TAG = "ui";

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_28);

extern const char layout_json_start[] asm("_binary_layout_json_start");
extern const char layout_json_end[] asm("_binary_layout_json_end");

#define TOAST_MS 10000

typedef enum { MSG_NET, MSG_RELOAD, MSG_LAYOUT } msg_kind_t;

typedef struct {
    msg_kind_t kind;
    union {
        net_status_t net;
        struct {
            char *json;
            size_t len;
            layout_src_t src;
        } layout;
    };
} ui_msg_t;

static QueueHandle_t s_queue;
static lv_display_t *s_disp;
static layout_t s_layout;
static screen_view_t s_views[LAYOUT_MAX_SCREENS];
static int s_view_count;
static lv_obj_t *s_info;      /* setup or connecting screen */
static bool s_dashboard_shown;
static uint8_t s_shown_hash[32];
static bool s_have_hash;

/* ---- dashboard --------------------------------------------------------- */

/* Parses json and, if it holds a usable screen, replaces the dashboard with
 * it. The old screens are deleted only after the new ones exist, so a bad
 * layout keeps what is on screen. A fetched layout that differs from the one
 * shown is also saved to flash. Takes ownership of json. */
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

    /* Static: a layout plus its views is several KB, too much for the stack. */
    static layout_t fresh;
    static screen_view_t views[LAYOUT_MAX_SCREENS];
    memset(views, 0, sizeof(views));
    if (!layout_parse(json, len, &fresh)) {
        free(json);
        if (src == LAYOUT_SRC_FETCHED) {
            char msg[80];
            snprintf(msg, sizeof(msg), "invalid layout: %s", layout_last_error());
            fetch_report_result(false, msg);
        }
        ESP_LOGE(TAG, "%s layout rejected: %s", layout_src_name(src), layout_last_error());
        return false;
    }

    lvgl_port_lock(0);
    widgets_build(s_disp, &fresh, views);
    for (int i = 0; i < fresh.screen_count; i++) {
        widgets_fill_placeholders(&views[i]);
    }
    if (s_dashboard_shown) {
        lv_screen_load(views[0].screen);
    }
    widgets_destroy(s_views, s_view_count);
    lvgl_port_unlock();

    layout_free(&s_layout);
    s_layout = fresh;
    memcpy(s_views, views, sizeof(views));
    s_view_count = fresh.screen_count;
    /* views point at widget definitions inside s_layout, which moved */
    for (int sc = 0; sc < s_view_count; sc++) {
        for (int i = 0; i < s_views[sc].count; i++) {
            s_views[sc].widgets[i].def = &s_layout.screens[sc].widgets[i];
        }
    }
    memcpy(s_shown_hash, hash, sizeof(hash));
    s_have_hash = true;

    if (src == LAYOUT_SRC_FETCHED) {
        store_save(json, len);
        fetch_report_result(true, NULL);
    }
    fetch_report_shown(src, len);
    free(json);
    ESP_LOGI(TAG, "%s layout shown (%u bytes), free heap %u", layout_src_name(src),
             (unsigned)len, (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
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
    lv_qrcode_set_size(qr, 160);
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
    lv_screen_load(s_views[0].screen);
    s_dashboard_shown = true;
    if (s_info) {
        lv_obj_delete(s_info);
        s_info = NULL;
    }

    /* Address toast on the top layer, gone after TOAST_MS. */
    lv_obj_t *toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(toast, 240, LV_SIZE_CONTENT);
    lv_obj_align(toast, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x2c2c2e), 0);
    lv_obj_set_style_border_width(toast, 0, 0);
    lv_obj_set_style_radius(toast, 12, 0);
    lv_obj_set_style_pad_all(toast, 12, 0);
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(toast, LV_FLEX_FLOW_COLUMN);
    label(toast, &lv_font_montserrat_20, 0xffffff, "tavle.local");
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
    widgets_fill_placeholders(view);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "bench all widgets changing @ %d MHz: avg %.1f ms (%.1f fps), worst %.1f ms",
             CONFIG_TAVLE_LCD_SPI_MHZ, total / 1000.0 / update_runs, 1e6 * update_runs / total,
             worst / 1000.0);
}
#endif

/* ---- task -------------------------------------------------------------- */

static void ui_task(void *arg)
{
    /* LVGL renders on the stack of whoever holds the lock, so all UI work
     * happens here rather than on small event or HTTP stacks. */
    show_offline_layout();
#if CONFIG_TAVLE_BENCH
    if (s_view_count) {
        bench();
    }
#endif

    ui_msg_t msg;
    for (;;) {
        if (!xQueueReceive(s_queue, &msg, portMAX_DELAY)) {
            continue;
        }
        if (msg.kind == MSG_NET) {
            on_net(&msg.net);
        } else if (msg.kind == MSG_RELOAD) {
            s_have_hash = false; /* rebuild even if unchanged */
            show_offline_layout();
        } else if (msg.kind == MSG_LAYOUT) {
            apply_layout(msg.layout.json, msg.layout.len, msg.layout.src);
        }
        memset(&msg, 0, sizeof(msg)); /* hotspot password */
    }
}

esp_err_t ui_start(lv_display_t *disp)
{
    s_disp = disp;
    s_queue = xQueueCreate(6, sizeof(ui_msg_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }

    lvgl_port_lock(0);
    lv_obj_t *scr = info_screen();
    label(scr, &lv_font_montserrat_28, 0xffffff, "tavle");
    show_info(scr);
    lvgl_port_unlock();

    return xTaskCreate(ui_task, "ui", 8192, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
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

void ui_request_reload(void)
{
    ui_msg_t msg = {.kind = MSG_RELOAD};
    xQueueSend(s_queue, &msg, pdMS_TO_TICKS(100));
}
