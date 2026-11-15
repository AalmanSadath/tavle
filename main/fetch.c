#include "fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "net.h"
#include "png.h"
#include "settings.h"
#include "store.h"
#include "ui.h"

static const char *TAG = "fetch";

#define TIMEOUT_MS     10000
#define MAX_REDIRECTS  3

typedef enum { REQ_FETCH, REQ_RELOAD, REQ_SETTINGS, REQ_IMAGE } fetch_req_t;

#define IMAGE_JOBS_MAX  4
#define IMAGE_MAX_BYTES (256 * 1024)

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

/* Opens url and reads the response headers, following up to MAX_REDIRECTS
 * redirects. Returns a client positioned at a 200 response body, or NULL
 * with a reason in err. */
static esp_http_client_handle_t open_url(const char *url, char *err, size_t err_len)
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
            goto fail;
        }
        if (esp_http_client_fetch_headers(h) < 0) {
            snprintf(err, err_len, "no response");
            goto fail;
        }
        code = esp_http_client_get_status_code(h);
        if (!is_redirect(code)) {
            break;
        }
        if (hop == MAX_REDIRECTS) {
            snprintf(err, err_len, "more than %d redirects", MAX_REDIRECTS);
            goto fail;
        }
        esp_http_client_flush_response(h, NULL);
        if (esp_http_client_set_redirection(h) != ESP_OK) {
            snprintf(err, err_len, "bad redirect");
            goto fail;
        }
        esp_http_client_close(h);
    }
    if (code == 200) {
        return h;
    }
    snprintf(err, err_len, "HTTP %d", code);

fail:
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return NULL;
}

static void close_url(esp_http_client_handle_t h)
{
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
}

/* Downloads url into a malloc'd buffer of at most STORE_MAX_LAYOUT bytes.
 * On failure returns NULL and writes a reason to err. */
static char *download(const char *url, size_t *len_out, char *err, size_t err_len)
{
    esp_http_client_handle_t h = open_url(url, err, err_len);
    if (!h) {
        return NULL;
    }
    char *buf = NULL;
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
    close_url(h);
    return buf;
}

/* ---- images ------------------------------------------------------------ */

typedef struct {
    bool used;
    unsigned gen;
    int slot;
    char *url;
    int max_w, max_h;
    uint32_t bg;
} image_job_t;

static image_job_t s_jobs[IMAGE_JOBS_MAX];

void fetch_image(unsigned gen, int slot, const char *url, int max_w, int max_h, uint32_t bg)
{
    char *copy = strdup(url);
    if (!copy) {
        ui_image_ready(gen, slot, NULL, NULL);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    image_job_t *job = NULL;
    for (int i = 0; i < IMAGE_JOBS_MAX && !job; i++) {
        if (s_jobs[i].used && s_jobs[i].slot == slot) {
            job = &s_jobs[i]; /* a newer picture for the same widget wins */
        }
    }
    for (int i = 0; i < IMAGE_JOBS_MAX && !job; i++) {
        if (!s_jobs[i].used) {
            job = &s_jobs[i];
        }
    }
    if (job) {
        free(job->url);
        *job = (image_job_t){.used = true, .gen = gen, .slot = slot, .url = copy,
                             .max_w = max_w, .max_h = max_h, .bg = bg};
    }
    xSemaphoreGive(s_lock);
    if (!job) {
        free(copy);
        ui_image_ready(gen, slot, NULL, NULL);
        return;
    }
    post(REQ_IMAGE);
}

/* Opens a plain http:// URL with a bare HTTP/1.0 request: a picture needs
 * almost all free memory, and the full HTTP client costs several KB more.
 * Returns a socket positioned at the body of a 200 response, or -1 with a
 * reason in err. Body bytes read with the headers go to *extra. */
static int open_plain(const char *url, char *extra, int *extra_len, char *err, size_t err_len)
{
    char host[64];
    const char *path;
    int port = 80;
    if (strncmp(url, "http://", 7) != 0) {
        snprintf(err, err_len, "pictures must use http://");
        return -1;
    }
    const char *h = url + 7;
    size_t n = strcspn(h, ":/");
    if (n == 0 || n >= sizeof(host)) {
        snprintf(err, err_len, "bad address");
        return -1;
    }
    memcpy(host, h, n);
    host[n] = '\0';
    path = h + n;
    if (*path == ':') {
        port = atoi(path + 1);
        path += 1 + strspn(path + 1, "0123456789");
    }
    if (*path != '/' || port < 1 || port > 65535) {
        snprintf(err, err_len, "bad address");
        return -1;
    }

    char port_s[8];
    snprintf(port_s, sizeof(port_s), "%d", port);
    const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *ai = NULL;
    if (getaddrinfo(host, port_s, &hints, &ai) != 0 || !ai) {
        snprintf(err, err_len, "cannot find %.40s", host);
        return -1;
    }
    int sock = socket(ai->ai_family, ai->ai_socktype, 0);
    if (sock < 0) {
        freeaddrinfo(ai);
        snprintf(err, err_len, "out of sockets");
        return -1;
    }
    const struct timeval tv = {.tv_sec = TIMEOUT_MS / 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int c = connect(sock, ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(ai);
    if (c != 0) {
        snprintf(err, err_len, "cannot connect to %.40s", host);
        close(sock);
        return -1;
    }

    char req[384];
    int len = snprintf(req, sizeof(req),
                       "GET %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: tavle\r\n\r\n", path, host,
                       port);
    if (len >= (int)sizeof(req) || send(sock, req, len, 0) != len) {
        snprintf(err, err_len, len >= (int)sizeof(req) ? "address too long" : "send failed");
        close(sock);
        return -1;
    }

    /* headers, read into req; whatever follows them is body */
    int got = 0;
    char *end = NULL;
    while (!end) {
        if (got == (int)sizeof(req) - 1) {
            snprintf(err, err_len, "response headers too long");
            close(sock);
            return -1;
        }
        int r = recv(sock, req + got, sizeof(req) - 1 - got, 0);
        if (r <= 0) {
            snprintf(err, err_len, "no response");
            close(sock);
            return -1;
        }
        got += r;
        req[got] = '\0';
        end = strstr(req, "\r\n\r\n");
    }
    int code = 0;
    if (sscanf(req, "HTTP/%*d.%*d %d", &code) != 1 || code != 200) {
        snprintf(err, err_len, "HTTP %d", code);
        close(sock);
        return -1;
    }
    end += 4;
    *extra_len = got - (int)(end - req);
    memcpy(extra, end, *extra_len);
    return sock;
}

typedef struct {
    int sock;
    char *head; /* body bytes that came with the headers */
    int head_len, head_pos;
    size_t total;
    bool too_big;
} picture_reader_t;

static int read_picture(void *ctx, uint8_t *buf, size_t len)
{
    picture_reader_t *r = ctx;
    int n;
    if (r->head_pos < r->head_len) {
        n = r->head_len - r->head_pos;
        n = n < (int)len ? n : (int)len;
        memcpy(buf, r->head + r->head_pos, n);
        r->head_pos += n;
    } else {
        n = recv(r->sock, buf, len, 0);
    }
    if (n > 0 && (r->total += n) > IMAGE_MAX_BYTES) {
        r->too_big = true;
        return -1;
    }
    return n;
}

static png_picture_t *download_png(const image_job_t *job, char *err, size_t err_len)
{
    char head[512];
    picture_reader_t r = {.head = head};
    r.sock = open_plain(job->url, head, &r.head_len, err, err_len);
    if (r.sock < 0) {
        return NULL;
    }
    const char *why = NULL;
    png_picture_t *pic = png_decode(read_picture, &r, job->max_w, job->max_h, job->bg, &why);
    close(r.sock);
    if (!pic) {
        if (r.too_big) {
            snprintf(err, err_len, "image larger than %d bytes", IMAGE_MAX_BYTES);
        } else {
            snprintf(err, err_len, "%s", why);
        }
    }
    return pic;
}

static void do_images(void)
{
    for (;;) {
        image_job_t job = {0};
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < IMAGE_JOBS_MAX; i++) {
            if (s_jobs[i].used) {
                job = s_jobs[i];
                s_jobs[i] = (image_job_t){0};
                break;
            }
        }
        xSemaphoreGive(s_lock);
        if (!job.used) {
            return;
        }

        char err[64] = "";
        int64_t t0 = esp_timer_get_time();
        png_picture_t *pic = download_png(&job, err, sizeof(err));
        if (pic) {
            ESP_LOGI(TAG, "image %dx%d in %lld ms, free heap %u", pic->w, pic->h,
                     (esp_timer_get_time() - t0) / 1000,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        } else {
            ESP_LOGW(TAG, "image %s (%dx%d): %s, free heap %u, largest block %u", job.url,
                     job.max_w, job.max_h, err,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
        ui_image_ready(job.gen, job.slot, job.url, pic); /* takes url and pic */
    }
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
        if (req != REQ_IMAGE) {
            do_fetch(req == REQ_RELOAD);
        }
        /* Pictures share this task with layout fetches, so the two big
         * memory users never run at the same time. */
        do_images();
    }
}

esp_err_t fetch_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(6, sizeof(fetch_req_t));
    const esp_timer_create_args_t t = {.callback = on_timer, .name = "layout_fetch"};
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_timer));
    /* TLS needs a deep stack. */
    return xTaskCreate(fetch_task, "fetch", 8192, NULL, 3, NULL) == pdPASS ? ESP_OK
                                                                          : ESP_ERR_NO_MEM;
}
