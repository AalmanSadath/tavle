#include "hwcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

static const char *TAG = "hwcfg";

/* One sector in the "storage" partition, after the two layout slots
 * (store.c). tools/tavle.py writes the same layout with esptool. */
#define HWCFG_OFFSET 0x20000
#define HWCFG_SECTOR 0x1000
#define MAGIC        0x68766174u /* "tavh" */

typedef struct {
    uint32_t magic;
    uint32_t len;
    uint32_t crc;
    uint32_t reserved;
} hdr_t;

static hwcfg_t s_cfg = TAVLE_DEFAULT_HWCFG;
static bool s_saved;
static char s_problem[112];

static const int s_reserved[] = TAVLE_RESERVED_PINS;

static bool reserved(int pin)
{
    for (size_t i = 0; i < sizeof(s_reserved) / sizeof(s_reserved[0]); i++) {
        if (s_reserved[i] == pin) {
            return true;
        }
    }
    return false;
}

/* Reads pins.<name>; *pin stays -1 when absent and the pin is optional. */
static bool get_pin(const cJSON *pins, const char *name, bool required, bool output, int *pin,
                    char *err, size_t errlen)
{
    *pin = -1;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(pins, name);
    if (!v || (cJSON_IsNumber(v) && v->valuedouble == -1)) {
        if (required) {
            snprintf(err, errlen, "pins.%s is required", name);
            return false;
        }
        return true;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble != (int)v->valuedouble) {
        snprintf(err, errlen, "pins.%s: not a whole number", name);
        return false;
    }
    int n = (int)v->valuedouble;
    if (output ? !GPIO_IS_VALID_OUTPUT_GPIO(n) : !GPIO_IS_VALID_GPIO(n)) {
        snprintf(err, errlen, "pins.%s: GPIO %d does not exist on the %s", name, n,
                 TAVLE_CHIP_NAME);
        return false;
    }
    if (reserved(n)) {
        snprintf(err, errlen, "pins.%s: GPIO %d is taken by flash, PSRAM or USB", name, n);
        return false;
    }
    *pin = n;
    return true;
}

static bool get_bool(const cJSON *o, const char *name, bool *out, char *err, size_t errlen)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, name);
    *out = false;
    if (!v) {
        return true;
    }
    if (!cJSON_IsBool(v)) {
        snprintf(err, errlen, "display.%s: not true or false", name);
        return false;
    }
    *out = cJSON_IsTrue(v);
    return true;
}

esp_err_t hwcfg_parse(const char *json, size_t len, hwcfg_t *out, char *err, size_t errlen)
{
    err[0] = '\0';
    hwcfg_t c = {0};
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        snprintf(err, errlen, "not a JSON object");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    bool ok = true;

    const cJSON *board = cJSON_GetObjectItemCaseSensitive(root, "board");
    if (cJSON_IsString(board)) {
        snprintf(c.board, sizeof(c.board), "%s", board->valuestring);
    } else {
        snprintf(c.board, sizeof(c.board), "custom");
    }

    const cJSON *disp = cJSON_GetObjectItemCaseSensitive(root, "display");
    const cJSON *drv = cJSON_GetObjectItemCaseSensitive(disp, "driver");
    if (!cJSON_IsObject(disp) || !cJSON_IsString(drv)) {
        snprintf(err, errlen, "display.driver is required");
        ok = false;
    } else if (!display_known(drv->valuestring)) {
        snprintf(err, errlen, "display.driver: no such driver in this firmware");
        ok = false;
    } else {
        snprintf(c.display, sizeof(c.display), "%s", drv->valuestring);
        const cJSON *mhz = cJSON_GetObjectItemCaseSensitive(disp, "spi_mhz");
        c.spi_mhz = 40;
        if (mhz) {
            if (!cJSON_IsNumber(mhz) || mhz->valuedouble < 1 || mhz->valuedouble > 80) {
                snprintf(err, errlen, "display.spi_mhz: 1 to 80");
                ok = false;
            } else {
                c.spi_mhz = (int)mhz->valuedouble;
            }
        }
        ok = ok && get_bool(disp, "bgr", &c.bgr, err, errlen) &&
             get_bool(disp, "invert", &c.invert, err, errlen) &&
             get_bool(disp, "mirror_x", &c.mirror_x, err, errlen) &&
             get_bool(disp, "mirror_y", &c.mirror_y, err, errlen);
    }

    const cJSON *pins = cJSON_GetObjectItemCaseSensitive(root, "pins");
    if (ok && !cJSON_IsObject(pins)) {
        snprintf(err, errlen, "pins is required");
        ok = false;
    }
    ok = ok && get_pin(pins, "sck", true, true, &c.sck, err, errlen) &&
         get_pin(pins, "mosi", true, true, &c.mosi, err, errlen) &&
         get_pin(pins, "miso", false, false, &c.miso, err, errlen) &&
         get_pin(pins, "cs", true, true, &c.cs, err, errlen) &&
         get_pin(pins, "dc", true, true, &c.dc, err, errlen) &&
         get_pin(pins, "rst", false, true, &c.rst, err, errlen) &&
         get_pin(pins, "bl", false, true, &c.bl, err, errlen) &&
         get_pin(pins, "button", false, false, &c.button, err, errlen);

    if (ok) {
        const struct { const char *name; int pin; } all[] = {
            {"sck", c.sck}, {"mosi", c.mosi}, {"miso", c.miso}, {"cs", c.cs},
            {"dc", c.dc},   {"rst", c.rst},   {"bl", c.bl},     {"button", c.button},
        };
        const int n = sizeof(all) / sizeof(all[0]);
        for (int i = 0; ok && i < n; i++) {
            for (int j = i + 1; ok && j < n; j++) {
                if (all[i].pin >= 0 && all[i].pin == all[j].pin) {
                    snprintf(err, errlen, "pins.%s and pins.%s are both GPIO %d", all[i].name,
                             all[j].name, all[i].pin);
                    ok = false;
                }
            }
        }
    }

    const cJSON *tx = cJSON_GetObjectItemCaseSensitive(root, "wifi_tx_power_qdbm");
    if (ok && tx) {
        if (!cJSON_IsNumber(tx) || tx->valuedouble < 0 || tx->valuedouble > 84) {
            snprintf(err, errlen, "wifi_tx_power_qdbm: 0 to 84");
            ok = false;
        } else {
            c.tx_power_qdbm = (int)tx->valuedouble;
        }
    }

    cJSON_Delete(root);
    if (!ok) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = c;
    return ESP_OK;
}

static const esp_partition_t *partition(void)
{
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (!p || p->size < HWCFG_OFFSET + HWCFG_SECTOR) {
        return NULL;
    }
    return p;
}

void hwcfg_load(void)
{
    const esp_partition_t *p = partition();
    hdr_t h;
    char *buf = NULL;
    if (!p || esp_partition_read(p, HWCFG_OFFSET, &h, sizeof(h)) != ESP_OK) {
        snprintf(s_problem, sizeof(s_problem), "no storage partition");
    } else if (h.magic != MAGIC) {
        /* nothing saved: the normal case before the first config */
    } else if (h.len == 0 || h.len > HWCFG_JSON_MAX || !(buf = malloc(h.len)) ||
               esp_partition_read(p, HWCFG_OFFSET + sizeof(h), buf, h.len) != ESP_OK ||
               esp_rom_crc32_le(0, (const uint8_t *)buf, h.len) != h.crc) {
        snprintf(s_problem, sizeof(s_problem), "saved config is damaged");
    } else {
        hwcfg_t c;
        char err[80];
        if (hwcfg_parse(buf, h.len, &c, err, sizeof(err)) == ESP_OK) {
            s_cfg = c;
            s_saved = true;
        } else {
            snprintf(s_problem, sizeof(s_problem), "saved config: %s", err);
        }
    }
    free(buf);

    if (s_problem[0]) {
        ESP_LOGW(TAG, "%s; using the %s default", s_problem, TAVLE_VARIANT_NAME);
    }
    ESP_LOGI(TAG, "%s (%s): %s, SCK %d MOSI %d MISO %d CS %d DC %d RST %d BL %d, button %d",
             s_cfg.board, s_saved ? "saved" : "default", s_cfg.display, s_cfg.sck, s_cfg.mosi,
             s_cfg.miso, s_cfg.cs, s_cfg.dc, s_cfg.rst, s_cfg.bl, s_cfg.button);
}

const hwcfg_t *hwcfg(void)
{
    return &s_cfg;
}

bool hwcfg_is_saved(void)
{
    return s_saved;
}

const char *hwcfg_problem(void)
{
    return s_problem;
}

esp_err_t hwcfg_save(const char *json, size_t len, char *err, size_t errlen)
{
    hwcfg_t c;
    if (len > HWCFG_JSON_MAX) {
        snprintf(err, errlen, "config over %d bytes", HWCFG_JSON_MAX);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t e = hwcfg_parse(json, len, &c, err, errlen);
    if (e != ESP_OK) {
        return e;
    }
    const esp_partition_t *p = partition();
    if (!p) {
        snprintf(err, errlen, "no storage partition");
        return ESP_ERR_NOT_FOUND;
    }
    const hdr_t h = {
        .magic = MAGIC,
        .len = len,
        .crc = esp_rom_crc32_le(0, (const uint8_t *)json, len),
    };
    e = esp_partition_erase_range(p, HWCFG_OFFSET, HWCFG_SECTOR);
    if (e == ESP_OK) {
        e = esp_partition_write(p, HWCFG_OFFSET + sizeof(h), json, len);
    }
    if (e == ESP_OK) {
        e = esp_partition_write(p, HWCFG_OFFSET, &h, sizeof(h)); /* header last */
    }
    if (e != ESP_OK) {
        snprintf(err, errlen, "flash write failed: %s", esp_err_to_name(e));
    }
    return e;
}

cJSON *hwcfg_to_json(const hwcfg_t *c)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "board", c->board);
    cJSON *d = cJSON_AddObjectToObject(o, "display");
    cJSON_AddStringToObject(d, "driver", c->display);
    cJSON_AddNumberToObject(d, "spi_mhz", c->spi_mhz);
    cJSON_AddBoolToObject(d, "bgr", c->bgr);
    cJSON_AddBoolToObject(d, "invert", c->invert);
    cJSON_AddBoolToObject(d, "mirror_x", c->mirror_x);
    cJSON_AddBoolToObject(d, "mirror_y", c->mirror_y);
    cJSON *p = cJSON_AddObjectToObject(o, "pins");
    cJSON_AddNumberToObject(p, "sck", c->sck);
    cJSON_AddNumberToObject(p, "mosi", c->mosi);
    cJSON_AddNumberToObject(p, "miso", c->miso);
    cJSON_AddNumberToObject(p, "cs", c->cs);
    cJSON_AddNumberToObject(p, "dc", c->dc);
    cJSON_AddNumberToObject(p, "rst", c->rst);
    cJSON_AddNumberToObject(p, "bl", c->bl);
    cJSON_AddNumberToObject(p, "button", c->button);
    cJSON_AddNumberToObject(o, "wifi_tx_power_qdbm", c->tx_power_qdbm);
    return o;
}
