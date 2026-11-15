#include "timesync.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "time";

static char s_tz[49] = "UTC0";
static bool s_tz_pending = true;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void on_sync(struct timeval *tv)
{
    ESP_LOGI(TAG, "clock set from the network");
}

void timesync_start(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = on_sync;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP failed to start");
    }
}

bool timesync_valid(void)
{
    return time(NULL) > 1735689600; /* after 2025-01-01 */
}

void timesync_set_tz(const char *tz)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_tz, tz && tz[0] ? tz : "UTC0", sizeof(s_tz));
    s_tz_pending = true;
    portEXIT_CRITICAL(&s_mux);
}

/* setenv is not safe against a concurrent localtime, so the zone is only
 * changed on the task that formats the time. */
void timesync_apply_tz(void)
{
    char tz[sizeof(s_tz)];
    bool pending;
    portENTER_CRITICAL(&s_mux);
    pending = s_tz_pending;
    s_tz_pending = false;
    memcpy(tz, s_tz, sizeof(tz));
    portEXIT_CRITICAL(&s_mux);
    if (pending) {
        setenv("TZ", tz, 1);
        tzset();
        ESP_LOGI(TAG, "time zone %s", tz);
    }
}
