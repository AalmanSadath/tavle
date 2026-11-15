#include "src_moonraker.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"

static const char *TAG = "moonraker";

#define RX_MAX          (4 * 1024)
#define RETRY_INFO_US   (5 * 1000 * 1000)
#define BACKOFF_MIN_MS  2000
#define BACKOFF_MAX_MS  30000

enum { ID_IDENTIFY = 1, ID_INFO = 2, ID_SUBSCRIBE = 3 };

typedef struct {
    int index;
    char uri[96];
    char **paths;
    int npaths;
    esp_websocket_client_handle_t ws;
    esp_timer_handle_t retry;
    char *rx;
    int rx_len;
    bool rx_skip;
    int backoff_ms;
} mr_t;

static void send_json(mr_t *m, const char *method, int id, cJSON *params)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddStringToObject(req, "method", method);
    if (params) {
        cJSON_AddItemToObject(req, "params", params);
    }
    cJSON_AddNumberToObject(req, "id", id);
    char *s = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (s) {
        esp_websocket_client_send_text(m->ws, s, strlen(s), pdMS_TO_TICKS(2000));
        free(s);
    }
}

/* "heater_fan fan1.speed" -> object "heater_fan fan1", field path "speed" */
static bool split_path(const char *path, char *obj, size_t obj_len, const char **rest)
{
    const char *dot = strchr(path, '.');
    size_t n = dot ? (size_t)(dot - path) : strlen(path);
    if (n == 0 || n >= obj_len) {
        return false;
    }
    memcpy(obj, path, n);
    obj[n] = '\0';
    *rest = dot ? dot + 1 : NULL;
    return true;
}

static void subscribe(mr_t *m)
{
    cJSON *objects = cJSON_CreateObject();
    char obj[SRC_PATH_MAX];
    for (int i = 0; i < m->npaths; i++) {
        const char *path = m->paths[i];
        const char *rest;
        if (!split_path(path, obj, sizeof(obj), &rest)) {
            continue;
        }
        cJSON *fields = cJSON_GetObjectItemCaseSensitive(objects, obj);
        if (!rest) {
            /* whole object: null asks for every field */
            if (fields) {
                cJSON_DeleteItemFromObjectCaseSensitive(objects, obj);
            }
            cJSON_AddNullToObject(objects, obj);
            continue;
        }
        if (cJSON_IsNull(fields)) {
            continue;
        }
        if (!fields) {
            fields = cJSON_AddArrayToObject(objects, obj);
        }
        char field[SRC_PATH_MAX];
        size_t n = strcspn(rest, ".");
        snprintf(field, sizeof(field), "%.*s", (int)n, rest);
        bool dup = false;
        const cJSON *f;
        cJSON_ArrayForEach(f, fields) {
            dup |= strcmp(f->valuestring, field) == 0;
        }
        if (!dup) {
            cJSON_AddItemToArray(fields, cJSON_CreateString(field));
        }
    }
    cJSON *params = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "objects", objects);
    send_json(m, "printer.objects.subscribe", ID_SUBSCRIBE, params);
}

/* Reports every bound path present in a Moonraker status object. */
static void report_status(mr_t *m, const cJSON *status)
{
    char obj[SRC_PATH_MAX], seg[SRC_PATH_MAX];
    for (int i = 0; i < m->npaths; i++) {
        const char *rest;
        if (!split_path(m->paths[i], obj, sizeof(obj), &rest) || !rest) {
            continue;
        }
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(status, obj);
        while (v && rest && *rest) {
            size_t n = strcspn(rest, ".");
            snprintf(seg, sizeof(seg), "%.*s", (int)n, rest);
            v = cJSON_GetObjectItemCaseSensitive(v, seg);
            rest = rest[n] ? rest + n + 1 : NULL;
        }
        if (!v) {
            continue;
        }
        if (cJSON_IsNumber(v)) {
            sources_report_number(m->index, m->paths[i], v->valuedouble);
        } else if (cJSON_IsBool(v)) {
            sources_report_number(m->index, m->paths[i], cJSON_IsTrue(v) ? 1 : 0);
        } else if (cJSON_IsString(v)) {
            sources_report_text(m->index, m->paths[i], v->valuestring);
        }
    }
}

static void handle_message(mr_t *m, const char *json, int len)
{
    cJSON *msg = cJSON_ParseWithLength(json, len);
    if (!msg) {
        return;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(msg, "result");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(msg, "error");
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(msg, "method"));

    if (cJSON_IsNumber(id) && id->valueint == ID_INFO) {
        const char *state = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(result, "klippy_state"));
        if (state && strcmp(state, "ready") == 0) {
            subscribe(m);
        } else {
            char d[64];
            snprintf(d, sizeof(d), "klipper %s", state ? state : "not reachable");
            sources_report_state(m->index, SRC_OFFLINE, d);
            esp_timer_stop(m->retry);
            esp_timer_start_once(m->retry, RETRY_INFO_US);
        }
    } else if (cJSON_IsNumber(id) && id->valueint == ID_SUBSCRIBE) {
        if (error) {
            char d[64];
            const char *em = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message"));
            snprintf(d, sizeof(d), "subscribe failed: %s", em ? em : "error");
            sources_report_state(m->index, SRC_OFFLINE, d);
            esp_timer_stop(m->retry);
            esp_timer_start_once(m->retry, RETRY_INFO_US);
        } else {
            report_status(m, cJSON_GetObjectItemCaseSensitive(result, "status"));
            sources_report_state(m->index, SRC_ONLINE, "");
            ESP_LOGI(TAG, "subscribed to %d paths", m->npaths);
        }
    } else if (method && strcmp(method, "notify_status_update") == 0) {
        report_status(m, cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(msg, "params"), 0));
    } else if (method && strcmp(method, "notify_klippy_ready") == 0) {
        send_json(m, "server.info", ID_INFO, NULL); /* subscriptions end with klippy */
    } else if (method && (strcmp(method, "notify_klippy_disconnected") == 0 ||
                          strcmp(method, "notify_klippy_shutdown") == 0)) {
        sources_report_state(m->index, SRC_OFFLINE, "klipper not ready");
    }
    cJSON_Delete(msg);
}

static void on_retry(void *arg)
{
    mr_t *m = arg;
    if (esp_websocket_client_is_connected(m->ws)) {
        send_json(m, "server.info", ID_INFO, NULL);
    }
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    mr_t *m = arg;
    const esp_websocket_event_data_t *e = event_data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected to %s", m->uri);
        m->backoff_ms = BACKOFF_MIN_MS;
        esp_websocket_client_set_reconnect_timeout(m->ws, m->backoff_ms);
        sources_report_state(m->index, SRC_STARTING, "connected, waiting for klipper");
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "client_name", "tavle");
        cJSON_AddStringToObject(p, "version", "0.1");
        cJSON_AddStringToObject(p, "type", "other");
        cJSON_AddStringToObject(p, "url", "https://github.com/AalmanSadath/tavle");
        send_json(m, "server.connection.identify", ID_IDENTIFY, p);
        send_json(m, "server.info", ID_INFO, NULL);
        break;
    }

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR: {
        char d[64];
        int code = e ? e->error_handle.esp_ws_handshake_status_code : 0;
        if (code == 401 || code == 403) {
            snprintf(d, sizeof(d), "refused (HTTP %d): add tavle to trusted_clients", code);
        } else if (code) {
            snprintf(d, sizeof(d), "refused (HTTP %d)", code);
        } else {
            snprintf(d, sizeof(d), "cannot reach %s", m->uri + 5);
        }
        sources_report_state(m->index, SRC_OFFLINE, d);
        esp_timer_stop(m->retry);
        /* back off so an unreachable printer is not hammered */
        esp_websocket_client_set_reconnect_timeout(m->ws, m->backoff_ms);
        m->backoff_ms = m->backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : m->backoff_ms * 2;
        break;
    }

    case WEBSOCKET_EVENT_DATA:
        if (e->op_code != 0x1 && e->op_code != 0x0) {
            break; /* pings, pongs, close */
        }
        if (e->payload_offset == 0) {
            m->rx_len = 0;
            m->rx_skip = e->payload_len > RX_MAX;
            if (m->rx_skip) {
                ESP_LOGW(TAG, "message of %d bytes dropped", e->payload_len);
            }
        }
        if (m->rx_skip || e->payload_offset + e->data_len > RX_MAX) {
            break;
        }
        memcpy(m->rx + e->payload_offset, e->data_ptr, e->data_len);
        m->rx_len = e->payload_offset + e->data_len;
        if (m->rx_len == e->payload_len) {
            handle_message(m, m->rx, m->rx_len);
        }
        break;

    default:
        break;
    }
}

static bool host_ok(const char *h)
{
    size_t n = strlen(h);
    if (n == 0 || n > 63) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)h[i]) && h[i] != '.' && h[i] != '-') {
            return false;
        }
    }
    return true;
}

static void mr_stop(void *ctx);

static void *mr_start(int index, const cJSON *config, const char *const *paths, int npaths)
{
    const char *host = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(config, "host"));
    const cJSON *port_item = cJSON_GetObjectItemCaseSensitive(config, "port");
    int port = cJSON_IsNumber(port_item) ? port_item->valueint : 7125;
    if (!host || !host_ok(host) || port < 1 || port > 65535) {
        sources_report_state(index, SRC_OFFLINE, "needs a valid host and port");
        return NULL;
    }

    mr_t *m = calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }
    m->index = index;
    m->backoff_ms = BACKOFF_MIN_MS;
    snprintf(m->uri, sizeof(m->uri), "ws://%s:%d/websocket", host, port);
    m->rx = malloc(RX_MAX);
    m->paths = calloc(npaths, sizeof(char *));
    if (!m->rx || !m->paths) {
        mr_stop(m);
        return NULL;
    }
    for (int i = 0; i < npaths; i++) {
        m->paths[i] = strdup(paths[i]);
        if (!m->paths[i]) {
            mr_stop(m);
            return NULL;
        }
        m->npaths++;
    }

    const esp_timer_create_args_t t = {.callback = on_retry, .arg = m, .name = "mr_retry"};
    if (esp_timer_create(&t, &m->retry) != ESP_OK) {
        mr_stop(m);
        return NULL;
    }

    const esp_websocket_client_config_t cfg = {
        .uri = m->uri,
        .buffer_size = 1024,
        .task_stack = 5120,
        .task_name = "moonraker",
        .reconnect_timeout_ms = BACKOFF_MIN_MS,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 30,
        .user_agent = "tavle",
    };
    m->ws = esp_websocket_client_init(&cfg);
    if (!m->ws) {
        mr_stop(m);
        return NULL;
    }
    esp_websocket_register_events(m->ws, WEBSOCKET_EVENT_ANY, on_ws_event, m);
    if (esp_websocket_client_start(m->ws) != ESP_OK) {
        mr_stop(m);
        return NULL;
    }
    ESP_LOGI(TAG, "source %d: %s, %d paths", index, m->uri, m->npaths);
    return m;
}

static void mr_stop(void *ctx)
{
    mr_t *m = ctx;
    /* The websocket task can arm the retry timer, so stop the timer on both
     * sides of destroying the client. */
    if (m->retry) {
        esp_timer_stop(m->retry);
    }
    if (m->ws) {
        esp_websocket_client_destroy(m->ws); /* stops its task; no events after */
    }
    if (m->retry) {
        esp_timer_stop(m->retry);
        esp_timer_delete(m->retry);
    }
    for (int i = 0; i < m->npaths; i++) {
        free(m->paths[i]);
    }
    free(m->paths);
    free(m->rx);
    free(m);
}

const source_driver_t src_moonraker_driver = {
    .type = "moonraker",
    .start = mr_start,
    .stop = mr_stop,
};
