#include "web.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fetch.h"
#include "sources.h"
#include "timesync.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "web";

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[] asm("_binary_setup_html_end");

#define SETTINGS_BODY_MAX 1024
#define OTA_CHUNK         4096

static esp_timer_handle_t s_reboot_timer;

/* ---- request checks ---------------------------------------------------- */

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    return httpd_resp_sendstr(req, buf);
}

/* Basic auth when an admin password is set. The user name is ignored. */
static bool authorized(httpd_req_t *req)
{
    if (!settings_has_admin()) {
        return true;
    }

    char hdr[160];
    unsigned char decoded[128];
    size_t dlen = 0;
    bool ok = false;

    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncmp(hdr, "Basic ", 6) == 0 &&
        mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &dlen,
                              (const unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
        decoded[dlen] = '\0';
        char *colon = strchr((char *)decoded, ':');
        ok = colon && settings_check_admin(colon + 1);
    }
    memset(decoded, 0, sizeof(decoded));
    memset(hdr, 0, sizeof(hdr));

    if (!ok) {
        vTaskDelay(pdMS_TO_TICKS(1000)); /* slows down guessing */
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"tavle\", charset=\"UTF-8\"");
        send_error(req, "401 Unauthorized", "login required");
    }
    return ok;
}

/* A cross-site page cannot add a custom header without a CORS preflight,
 * which this server never answers, so it cannot reach these routes. */
static bool same_origin_post(httpd_req_t *req, const char *content_type)
{
    char v[48];
    if (httpd_req_get_hdr_value_str(req, "X-Tavle", v, sizeof(v)) != ESP_OK ||
        strcmp(v, "1") != 0) {
        send_error(req, "403 Forbidden", "missing X-Tavle header");
        return false;
    }
    if (content_type) {
        if (httpd_req_get_hdr_value_str(req, "Content-Type", v, sizeof(v)) != ESP_OK ||
            strncmp(v, content_type, strlen(content_type)) != 0) {
            send_error(req, "415 Unsupported Media Type", "wrong content type");
            return false;
        }
    }
    return true;
}

static bool guard(httpd_req_t *req, const char *content_type)
{
    return authorized(req) && (req->method != HTTP_POST || same_origin_post(req, content_type));
}

static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, s);
    free(s);
    return err;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void reboot_soon(void)
{
    esp_timer_start_once(s_reboot_timer, 800 * 1000);
}

static void on_reboot_timer(void *arg)
{
    esp_restart();
}

/* ---- routes ------------------------------------------------------------ */

static esp_err_t page_get(httpd_req_t *req)
{
    if (!guard(req, NULL)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    size_t len = setup_html_end - setup_html_start;
    while (len && setup_html_start[len - 1] == '\0') {
        len--;
    }
    return httpd_resp_send(req, setup_html_start, len);
}

static esp_err_t status_get(httpd_req_t *req)
{
    if (!guard(req, NULL)) {
        return ESP_OK;
    }
    net_status_t n;
    net_get_status(&n);
    settings_t s;
    settings_get(&s);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", net_state_name(n.state));
    cJSON_AddStringToObject(o, "ssid", n.ssid);
    cJSON_AddStringToObject(o, "ip", n.ip);
    cJSON_AddNumberToObject(o, "rssi", n.rssi);
    cJSON_AddBoolToObject(o, "hotspot", n.ap_active);
    cJSON_AddStringToObject(o, "layout_url", s.layout_url);
    cJSON_AddNumberToObject(o, "interval", s.interval_min);
    cJSON_AddBoolToObject(o, "has_password", s.has_admin);
    cJSON_AddStringToObject(o, "version", esp_app_get_description()->version);
    cJSON *b = cJSON_AddObjectToObject(o, "board");
    cJSON_AddStringToObject(b, "name", board_profile.name);
    cJSON_AddNumberToObject(b, "max_sources", board_profile.max_sources);
    cJSON_AddNumberToObject(b, "max_images", board_profile.max_images);
    cJSON_AddBoolToObject(b, "keep_running", board_profile.keep_running);
    cJSON_AddStringToObject(o, "slot", esp_ota_get_running_partition()->label);
    cJSON_AddNumberToObject(o, "free_heap", heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    cJSON_AddNumberToObject(o, "largest_block", heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    layout_status_t ls;
    fetch_get_status(&ls);
    cJSON_AddStringToObject(o, "layout_source", layout_src_name(ls.source));
    cJSON_AddNumberToObject(o, "layout_bytes", ls.bytes);
    cJSON_AddNumberToObject(o, "layout_age_s",
                            ls.last_ok_us ? (esp_timer_get_time() - ls.last_ok_us) / 1000000 : -1);
    cJSON_AddStringToObject(o, "layout_error", ls.error);

    cJSON_AddStringToObject(o, "tz", s.tz);
    char now[32] = "";
    if (timesync_valid()) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm); /* TZ is only changed on the UI task; a stale zone here is harmless */
        strftime(now, sizeof(now), "%Y-%m-%d %H:%M", &tm);
    }
    cJSON_AddStringToObject(o, "time", now);

    source_status_t src[LAYOUT_MAX_SOURCES];
    int nsrc = sources_get_status(src, LAYOUT_MAX_SOURCES);
    cJSON *arr = cJSON_AddArrayToObject(o, "sources");
    for (int i = 0; i < nsrc; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", src[i].name);
        cJSON_AddStringToObject(e, "type", src[i].type);
        cJSON_AddStringToObject(e, "state", src_state_name(src[i].state));
        cJSON_AddStringToObject(e, "detail", src[i].detail);
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddNumberToObject(o, "min_free_heap", heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
    memset(&s, 0, sizeof(s));
    return send_json(req, o);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    if (!guard(req, NULL)) {
        return ESP_OK;
    }
    net_scan_entry_t list[16];
    int n = net_scan(list, 16);
    if (n < 0) {
        return send_error(req, "503 Service Unavailable", "scan failed, try again");
    }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", list[i].ssid);
        cJSON_AddNumberToObject(e, "rssi", list[i].rssi);
        cJSON_AddBoolToObject(e, "open", list[i].open);
        cJSON_AddItemToArray(arr, e);
    }
    return send_json(req, arr);
}

static const char *str_field(const cJSON *o, const char *key, bool *present)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    *present = it != NULL;
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static esp_err_t settings_post(httpd_req_t *req)
{
    if (!guard(req, "application/json")) {
        return ESP_OK;
    }
    if (req->content_len == 0 || req->content_len > SETTINGS_BODY_MAX) {
        return send_error(req, "413 Content Too Large", "body too large");
    }

    char *body = malloc(req->content_len + 1);
    if (!body) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';
    cJSON *in = cJSON_Parse(body);
    memset(body, 0, got);
    free(body);
    if (!cJSON_IsObject(in)) {
        cJSON_Delete(in);
        return send_error(req, "400 Bad Request", "body must be a JSON object");
    }

    settings_t s;
    settings_get(&s);
    const char *err = NULL;
    bool wifi_changed = false;
    bool present;

    const char *ssid = str_field(in, "ssid", &present);
    if (present) {
        bool pass_present;
        const char *pass = str_field(in, "wifi_password", &pass_present);
        if (!ssid || !ssid[0] || strlen(ssid) > SETTINGS_SSID_MAX) {
            err = "network name must be 1 to 32 bytes";
        } else if (pass_present &&
                   (!pass || (pass[0] && (strlen(pass) < 8 || strlen(pass) > SETTINGS_WIFI_PASS_MAX)))) {
            err = "wi-fi password must be empty or 8 to 63 characters";
        } else {
            /* Same network and no password given: keep the stored one. */
            bool keep = !pass_present && strcmp(ssid, s.ssid) == 0;
            strlcpy(s.ssid, ssid, sizeof(s.ssid));
            if (!keep) {
                strlcpy(s.wifi_pass, pass ? pass : "", sizeof(s.wifi_pass));
            }
            wifi_changed = true;
        }
    }

    const settings_t before = s;
    const char *url = str_field(in, "layout_url", &present);
    if (!err && present) {
        if (!url || strlen(url) > SETTINGS_URL_MAX ||
            (url[0] && strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
            err = "layout url must be empty or start with http:// or https://";
        } else {
            strlcpy(s.layout_url, url, sizeof(s.layout_url));
        }
    }

    const cJSON *interval = cJSON_GetObjectItemCaseSensitive(in, "interval");
    if (!err && interval) {
        if (!cJSON_IsNumber(interval) || interval->valuedouble < SETTINGS_INTERVAL_MIN ||
            interval->valuedouble > SETTINGS_INTERVAL_MAX) {
            err = "interval must be 1 to 1440 minutes";
        } else {
            s.interval_min = (uint16_t)interval->valuedouble;
        }
    }

    const char *tz = str_field(in, "tz", &present);
    if (!err && present) {
        bool ok = tz && tz[0] && strlen(tz) <= SETTINGS_TZ_MAX;
        for (const char *c = tz; ok && *c; c++) {
            ok = isalnum((unsigned char)*c) || strchr("+-,./:<>", *c);
        }
        if (!ok) {
            err = "time zone must be a POSIX TZ string, for example AEST-10AEDT,M10.1.0,M4.1.0/3";
        } else {
            strlcpy(s.tz, tz, sizeof(s.tz));
        }
    }

    /* Changing or clearing an existing password already needed the current
     * one, because every route is behind the login when it is set. */
    const char *admin = str_field(in, "admin_password", &present);
    if (!err && present) {
        if (!admin || (admin[0] && (strlen(admin) < SETTINGS_ADMIN_MIN ||
                                    strlen(admin) > SETTINGS_ADMIN_MAX))) {
            err = "admin password must be empty or 8 to 64 characters";
        } else {
            settings_set_admin(&s, admin);
        }
    }

    cJSON_Delete(in); /* also drops the passwords it held */

    esp_err_t res;
    if (err) {
        res = send_error(req, "400 Bad Request", err);
    } else if (settings_save(&s) != ESP_OK) {
        res = send_error(req, "500 Internal Server Error", "could not save settings");
    } else {
        ESP_LOGI(TAG, "settings saved%s", wifi_changed ? ", rejoining wi-fi" : "");
        res = send_ok(req);
        if (wifi_changed) {
            net_apply_settings();
        }
        if (strcmp(before.layout_url, s.layout_url) != 0 || before.interval_min != s.interval_min) {
            fetch_settings_changed();
        }
        if (strcmp(before.tz, s.tz) != 0) {
            timesync_set_tz(s.tz);
        }
    }
    memset(&s, 0, sizeof(s));
    return res;
}

static esp_err_t reload_post(httpd_req_t *req)
{
    if (!guard(req, NULL)) {
        return ESP_OK;
    }
    fetch_trigger();
    return send_ok(req);
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (!guard(req, NULL)) {
        return ESP_OK;
    }
    send_ok(req);
    reboot_soon();
    return ESP_OK;
}

static esp_err_t update_post(httpd_req_t *req)
{
    if (!guard(req, "application/octet-stream")) {
        return ESP_OK;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return send_error(req, "500 Internal Server Error", "no update slot");
    }
    if (req->content_len == 0 || req->content_len > part->size) {
        return send_error(req, "413 Content Too Large", "file is empty or larger than the slot");
    }

    char *buf = malloc(OTA_CHUNK);
    if (!buf) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        free(buf);
        return send_error(req, "500 Internal Server Error", "could not start update");
    }

    ESP_LOGI(TAG, "firmware upload: %u bytes into %s", (unsigned)req->content_len, part->label);
    size_t left = req->content_len;
    while (left > 0) {
        int r = httpd_req_recv(req, buf, left < OTA_CHUNK ? left : OTA_CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buf, r);
        if (err != ESP_OK) {
            break;
        }
        left -= r;
    }
    free(buf);

    if (err != ESP_OK) {
        esp_ota_abort(ota);
        ESP_LOGW(TAG, "firmware upload failed: %s", esp_err_to_name(err));
        return send_error(req, "400 Bad Request", "upload failed");
    }
    err = esp_ota_end(ota); /* checks image header, chip and SHA-256 */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "firmware rejected: %s", esp_err_to_name(err));
        return send_error(req, "400 Bad Request", "not a valid tavle firmware image");
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "could not select new firmware");
    }
    ESP_LOGI(TAG, "firmware accepted, rebooting into %s", part->label);
    send_ok(req);
    reboot_soon();
    return ESP_OK;
}

/* Captive portal: phones probe fixed URLs; anything unknown goes to the page
 * while the hotspot is up. */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t code)
{
    net_status_t n;
    net_get_status(&n);
    if (n.ap_active) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        /* iOS needs a body to detect the portal. */
        return httpd_resp_sendstr(req, "tavle setup");
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    return ESP_FAIL;
}

esp_err_t web_start(void)
{
    const esp_timer_create_args_t t = {.callback = on_reboot_timer, .name = "reboot"};
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_reboot_timer));

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 10;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;

    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_get},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post},
        {.uri = "/api/reload", .method = HTTP_POST, .handler = reload_post},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post},
        {.uri = "/api/update", .method = HTTP_POST, .handler = update_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "setup page on port 80");
    return ESP_OK;
}
