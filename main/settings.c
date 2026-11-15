#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NS = "tavle";

static settings_t s_cur;
static SemaphoreHandle_t s_lock;

static void hash_password(const uint8_t salt[16], const char *password, uint8_t out[32])
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, salt, 16);
    mbedtls_sha256_update(&ctx, (const uint8_t *)password, strlen(password));
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

static void load_str(nvs_handle_t h, const char *key, char *buf, size_t size)
{
    size_t len = size;
    if (nvs_get_str(h, key, buf, &len) != ESP_OK) {
        buf[0] = '\0';
    }
}

esp_err_t settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition unusable, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    memset(&s_cur, 0, sizeof(s_cur));
    s_cur.interval_min = SETTINGS_INTERVAL_DEF;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        load_str(h, "ssid", s_cur.ssid, sizeof(s_cur.ssid));
        load_str(h, "wifi_pass", s_cur.wifi_pass, sizeof(s_cur.wifi_pass));
        load_str(h, "layout_url", s_cur.layout_url, sizeof(s_cur.layout_url));
        uint16_t n;
        if (nvs_get_u16(h, "interval", &n) == ESP_OK &&
            n >= SETTINGS_INTERVAL_MIN && n <= SETTINGS_INTERVAL_MAX) {
            s_cur.interval_min = n;
        }
        size_t salt_len = sizeof(s_cur.admin_salt), hash_len = sizeof(s_cur.admin_hash);
        s_cur.has_admin =
            nvs_get_blob(h, "admin_salt", s_cur.admin_salt, &salt_len) == ESP_OK &&
            nvs_get_blob(h, "admin_hash", s_cur.admin_hash, &hash_len) == ESP_OK &&
            salt_len == sizeof(s_cur.admin_salt) && hash_len == sizeof(s_cur.admin_hash);
        nvs_close(h);
    }

    ESP_LOGI(TAG, "wifi %s, layout url %s, interval %u min, admin password %s",
             s_cur.ssid[0] ? "saved" : "not set", s_cur.layout_url[0] ? "set" : "not set",
             s_cur.interval_min, s_cur.has_admin ? "set" : "not set");
    return ESP_OK;
}

void settings_get(settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cur;
    xSemaphoreGive(s_lock);
}

esp_err_t settings_save(const settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, "ssid", s->ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "wifi_pass", s->wifi_pass);
    if (err == ESP_OK) err = nvs_set_str(h, "layout_url", s->layout_url);
    if (err == ESP_OK) err = nvs_set_u16(h, "interval", s->interval_min);
    if (err == ESP_OK) {
        if (s->has_admin) {
            err = nvs_set_blob(h, "admin_salt", s->admin_salt, sizeof(s->admin_salt));
            if (err == ESP_OK) err = nvs_set_blob(h, "admin_hash", s->admin_hash, sizeof(s->admin_hash));
        } else {
            nvs_erase_key(h, "admin_salt");
            nvs_erase_key(h, "admin_hash");
        }
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cur = *s;
        xSemaphoreGive(s_lock);
    }
    return err;
}

void settings_set_admin(settings_t *s, const char *password)
{
    if (!password || !password[0]) {
        s->has_admin = false;
        memset(s->admin_salt, 0, sizeof(s->admin_salt));
        memset(s->admin_hash, 0, sizeof(s->admin_hash));
        return;
    }
    esp_fill_random(s->admin_salt, sizeof(s->admin_salt));
    hash_password(s->admin_salt, password, s->admin_hash);
    s->has_admin = true;
}

bool settings_has_admin(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool has = s_cur.has_admin;
    xSemaphoreGive(s_lock);
    return has;
}

bool settings_check_admin(const char *password)
{
    settings_t s;
    settings_get(&s);
    if (!s.has_admin) {
        return true;
    }
    if (!password) {
        return false;
    }

    uint8_t h[32];
    hash_password(s.admin_salt, password, h);
    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof(h); i++) {
        diff |= h[i] ^ s.admin_hash[i]; /* constant time */
    }
    return diff == 0;
}
