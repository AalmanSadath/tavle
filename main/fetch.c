#include "fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "net.h"
#include "settings.h"
#include "store.h"
#include "ui.h"

static const char *TAG = "fetch";

#define TIMEOUT_MS     10000
#define MAX_REDIRECTS  3

typedef enum { REQ_FETCH, REQ_RELOAD, REQ_SETTINGS } fetch_req_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_timer;
static layout_status_t s_status;
static uint16_t s_interval_min;

const char *layout_src_name(layout_src_t src)
{
    static const char *const names[] = {"built-in", "saved", "fetched"};
    return names[src];
}

static void post(fetch_req_t req)
{
    xQueueSend(s_queue, &req, 0);
}

void fetch_trigger(void) { post(REQ_RELOAD); }
void fetch_settings_changed(void) { post(REQ_SETTINGS); }
void fetch_network_up(void) { post(REQ_FETCH); }

void fetch_report_shown(layout_src_t source, size_t bytes)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.source = source;
    s_status.bytes = bytes;
    xSemaphoreGive(s_lock);
}

void fetch_report_result(bool ok, const char *error)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ok) {
        s_status.last_ok_us = esp_timer_get_time();
        s_status.error[0] = '\0';
    } else {
        strlcpy(s_status.error, error, sizeof(s_status.error));
    }
    xSemaphoreGive(s_lock);
    if (!ok) {
        ESP_LOGW(TAG, "layout not updated: %s", error);
    }
}

void fetch_get_status(layout_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

static void restart_timer(void)
{
    settings_t s;
    settings_get(&s);
    s_interval_min = s.interval_min;
    esp_timer_stop(s_timer);
    if (s.layout_url[0]) {
        esp_timer_start_periodic(s_timer, (int64_t)s_interval_min * 60 * 1000000);
    }
}

static bool is_redirect(int code)
{
    return code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
}

/* Downloads url into a malloc'd buffer of at most STORE_MAX_LAYOUT bytes.
 * On failure returns NULL and writes a reason to err. */
static char *download(const char *url, size_t *len_out, char *err, size_t err_len)
{
    const esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach, /* https: verify against the CA bundle */
        .disable_auto_redirect = true,              /* followed below, with a limit */
        .user_agent = "tavle",
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        snprintf(err, err_len, "out of memory");
        return NULL;
    }

    char *buf = NULL;
    int code = 0;
    for (int hop = 0;; hop++) {
        esp_err_t e = esp_http_client_open(h, 0);
        if (e != ESP_OK) {
            int tls_code = 0, tls_flags = 0;
            esp_http_client_get_and_clear_last_tls_error(h, &tls_code, &tls_flags);
            if (tls_flags) {
                /* the server certificate was refused */
                snprintf(err, err_len, "certificate not trusted");
            } else {
                snprintf(err, err_len, "cannot connect (%s)", esp_err_to_name(e));
            }
            goto done;
        }
        if (esp_http_client_fetch_headers(h) < 0) {
            snprintf(err, err_len, "no response");
            goto done;
        }
        code = esp_http_client_get_status_code(h);
        if (!is_redirect(code)) {
            break;
        }
        if (hop == MAX_REDIRECTS) {
            snprintf(err, err_len, "more than %d redirects", MAX_REDIRECTS);
            goto done;
        }
        esp_http_client_flush_response(h, NULL);
        if (esp_http_client_set_redirection(h) != ESP_OK) {
            snprintf(err, err_len, "bad redirect");
            goto done;
        }
        esp_http_client_close(h);
    }

    if (code != 200) {
        snprintf(err, err_len, "HTTP %d", code);
        goto done;
    }
    int64_t clen = esp_http_client_get_content_length(h);
    if (clen > STORE_MAX_LAYOUT) {
        snprintf(err, err_len, "layout is %lld bytes, limit %d", clen, STORE_MAX_LAYOUT);
        goto done;
    }

    buf = malloc(STORE_MAX_LAYOUT + 1);
    if (!buf) {
        snprintf(err, err_len, "out of memory");
        goto done;
    }
    size_t len = 0;
    for (;;) {
        /* Read one byte past the limit to notice a body that is too long. */
        int r = esp_http_client_read(h, buf + len, STORE_MAX_LAYOUT + 1 - len);
        if (r < 0) {
            snprintf(err, err_len, "read failed");
            free(buf);
            buf = NULL;
            goto done;
        }
        if (r == 0) {
            break;
        }
        len += r;
        if (len > STORE_MAX_LAYOUT) {
            snprintf(err, err_len, "layout larger than %d bytes", STORE_MAX_LAYOUT);
            free(buf);
            buf = NULL;
            goto done;
        }
    }
    if (len == 0) {
        snprintf(err, err_len, "empty response");
        free(buf);
        buf = NULL;
        goto done;
    }
    buf[len] = '\0';
    *len_out = len;

done:
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return buf;
}

static void do_fetch(bool rebuild_without_url)
{
    settings_t s;
    settings_get(&s);
    char url[SETTINGS_URL_MAX + 1];
    strlcpy(url, s.layout_url, sizeof(url));
    memset(&s, 0, sizeof(s));

    if (!url[0]) {
        if (rebuild_without_url) {
            ui_request_reload();
        }
        return;
    }

    net_status_t n;
    net_get_status(&n);
    if (n.state != NET_CONNECTED) {
        fetch_report_result(false, "not connected to wi-fi");
        return;
    }

    ESP_LOGI(TAG, "fetching %s", url);
    char err[80] = "";
    size_t len = 0;
    char *body = download(url, &len, err, sizeof(err));
    if (!body) {
        fetch_report_result(false, err);
        return;
    }
    ESP_LOGI(TAG, "got %u bytes", (unsigned)len);
    ui_set_layout(body, len, LAYOUT_SRC_FETCHED); /* takes ownership */
}

static void on_timer(void *arg)
{
    post(REQ_FETCH);
}

static void fetch_task(void *arg)
{
    restart_timer();
    fetch_req_t req;
    for (;;) {
        if (!xQueueReceive(s_queue, &req, portMAX_DELAY)) {
            continue;
        }
        if (req == REQ_SETTINGS) {
            restart_timer();
        }
        do_fetch(req == REQ_RELOAD);
    }
}

esp_err_t fetch_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(4, sizeof(fetch_req_t));
    const esp_timer_create_args_t t = {.callback = on_timer, .name = "layout_fetch"};
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_timer));
    /* TLS needs a deep stack. */
    return xTaskCreate(fetch_task, "fetch", 8192, NULL, 3, NULL) == pdPASS ? ESP_OK
                                                                          : ESP_ERR_NO_MEM;
}
