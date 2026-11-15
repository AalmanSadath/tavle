#include "net.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "dns_server.h" /* needs esp_netif.h first */
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "mdns.h"
#include "settings.h"

static const char *TAG = "net";

#define FALLBACK_AFTER_US   (60LL * 1000 * 1000)
#define RETRY_MIN_MS        2000
#define RETRY_MAX_MS        30000
#define AP_MAX_CLIENTS      2

typedef enum {
    EV_GOT_IP,
    EV_STA_DISCONNECTED,
    EV_RETRY,
    EV_FALLBACK,
    EV_AP_CLIENT_LEFT,
    EV_APPLY,
} net_event_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static net_status_t s_status;
static net_status_cb_t s_cb;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_timer_handle_t s_fallback_timer;
static esp_timer_handle_t s_retry_timer;
static dns_server_handle_t s_dns;
static int s_retry_ms = RETRY_MIN_MS;
static bool s_have_creds;
static bool s_scanning;

/* DHCP option 114 keeps a pointer to this string. */
static const char CAPTIVE_URI[] = "http://192.168.4.1/";

const char *net_state_name(net_state_t state)
{
    static const char *const names[] = {"setup", "connecting", "connected", "fallback"};
    return names[state];
}

static void post(net_event_t ev)
{
    xQueueSend(s_queue, &ev, 0);
}

static void notify(void)
{
    net_status_t copy;
    net_get_status(&copy);
    if (s_cb) {
        s_cb(&copy);
    }
}

static void set_state(net_state_t state)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = state;
    if (state != NET_CONNECTED) {
        s_status.ip[0] = '\0';
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "state %s", net_state_name(state));
}

static void make_ap_identity(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_status.ap_ssid, sizeof(s_status.ap_ssid), "tavle-%02x%02x", mac[4], mac[5]);

    /* No 0/O, 1/l/i, so it can be typed from the screen. */
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < 8; i++) {
        s_status.ap_pass[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    s_status.ap_pass[8] = '\0';
}

static void start_ap(void)
{
    if (s_status.ap_active) {
        return;
    }
    wifi_config_t ap = {
        .ap = {
            .channel = 1,
            .max_connection = AP_MAX_CLIENTS,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {.required = false},
        },
    };
    strlcpy((char *)ap.ap.ssid, s_status.ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_status.ap_ssid);
    strlcpy((char *)ap.ap.password, s_status.ap_pass, sizeof(ap.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                           (void *)CAPTIVE_URI, strlen(CAPTIVE_URI));
    esp_netif_dhcps_start(s_ap_netif);

    dns_server_config_t dns = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dns);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.ap_active = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "setup hotspot %s up", s_status.ap_ssid);
}

static void stop_ap(void)
{
    if (!s_status.ap_active) {
        return;
    }
    wifi_sta_list_t clients;
    if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num > 0) {
        ESP_LOGI(TAG, "hotspot kept up while %d client(s) are on it", clients.num);
        return;
    }
    if (s_dns) {
        stop_dns_server(s_dns);
        s_dns = NULL;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.ap_active = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "setup hotspot down");
}

static void connect_saved(void)
{
    settings_t s;
    settings_get(&s);
    s_have_creds = s.ssid[0] != '\0';

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_status.ssid, s.ssid, sizeof(s_status.ssid));
    xSemaphoreGive(s_lock);

    esp_timer_stop(s_retry_timer);
    esp_timer_stop(s_fallback_timer);
    esp_wifi_disconnect();

    if (!s_have_creds) {
        start_ap();
        set_state(NET_SETUP);
        return;
    }

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, s.ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, s.wifi_pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = s.wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    /* Also when the hotspot is already up (new credentials saved from it):
     * the screen says "connecting" and returns to the hotspot details if
     * the join does not work within the fallback delay. */
    s_retry_ms = RETRY_MIN_MS;
    esp_wifi_connect();
    esp_timer_start_once(s_fallback_timer, FALLBACK_AFTER_US);
    set_state(NET_CONNECTING);
    ESP_LOGI(TAG, "joining '%s'", s.ssid);
}

static void handle(net_event_t ev)
{
    switch (ev) {
    case EV_GOT_IP:
        esp_timer_stop(s_fallback_timer);
        esp_timer_stop(s_retry_timer);
        s_retry_ms = RETRY_MIN_MS;
        set_state(NET_CONNECTED);
        stop_ap();
        break;

    case EV_STA_DISCONNECTED:
        if (!s_have_creds || s_scanning) {
            break;
        }
        if (s_status.state == NET_CONNECTED) {
            set_state(NET_CONNECTING);
            esp_timer_start_once(s_fallback_timer, FALLBACK_AFTER_US);
        }
        /* While the hotspot is up, retry slowly: each attempt makes the radio
         * scan other channels, which disturbs phones on the hotspot. */
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer,
                             (s_status.ap_active ? RETRY_MAX_MS : s_retry_ms) * 1000LL);
        s_retry_ms = s_retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : s_retry_ms * 2;
        break;

    case EV_RETRY:
        if (s_have_creds && s_status.state != NET_CONNECTED) {
            esp_wifi_connect();
        }
        break;

    case EV_FALLBACK:
        if (s_status.state != NET_CONNECTED) {
            ESP_LOGW(TAG, "no connection for %lld s, starting setup hotspot",
                     FALLBACK_AFTER_US / 1000000);
            start_ap();
            set_state(NET_FALLBACK);
        }
        break;

    case EV_AP_CLIENT_LEFT:
        if (s_status.state == NET_CONNECTED) {
            stop_ap();
        }
        break;

    case EV_APPLY:
        connect_saved();
        break;
    }
    notify();
}

static void net_task(void *arg)
{
    connect_saved();
    notify();
    net_event_t ev;
    for (;;) {
        if (xQueueReceive(s_queue, &ev, portMAX_DELAY)) {
            handle(ev);
        }
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        post(EV_STA_DISCONNECTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        post(EV_AP_CLIENT_LEFT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&e->ip_info.ip));
        xSemaphoreGive(s_lock);
        post(EV_GOT_IP);
    }
}

static void on_timer(void *arg)
{
    post((net_event_t)(intptr_t)arg);
}

esp_err_t net_start(net_status_cb_t cb)
{
    s_cb = cb;
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(8, sizeof(net_event_t));
    make_ap_identity();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, "tavle");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); /* credentials live in settings */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
#if CONFIG_TAVLE_WIFI_TX_POWER_QDBM > 0
    /* Many small ESP32-C3 boards have a poorly matched antenna; at full
     * power the signal distorts and other devices cannot receive it. */
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(CONFIG_TAVLE_WIFI_TX_POWER_QDBM));
#endif

    const esp_timer_create_args_t fb = {.callback = on_timer, .arg = (void *)EV_FALLBACK,
                                        .name = "net_fallback"};
    const esp_timer_create_args_t rt = {.callback = on_timer, .arg = (void *)EV_RETRY,
                                        .name = "net_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&fb, &s_fallback_timer));
    ESP_ERROR_CHECK(esp_timer_create(&rt, &s_retry_timer));

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("tavle");
        mdns_instance_name_set("tavle");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    } else {
        ESP_LOGW(TAG, "mDNS failed to start");
    }

    xTaskCreate(net_task, "net", 4096, NULL, 5, NULL);
    return ESP_OK;
}

void net_get_status(net_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
    out->rssi = 0;
    wifi_ap_record_t ap;
    if (out->state == NET_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->rssi = ap.rssi;
    }
}

void net_apply_settings(void)
{
    post(EV_APPLY);
}

int net_scan(net_scan_entry_t *out, int max)
{
    /* A scan needs the station idle; joining attempts are paused for it. */
    s_scanning = true;
    if (s_status.state != NET_CONNECTED) {
        esp_wifi_disconnect();
    }
    const wifi_scan_config_t sc = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    s_scanning = false;
    if (s_have_creds && s_status.state != NET_CONNECTED) {
        post(EV_RETRY);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        esp_wifi_clear_ap_list();
        return -1;
    }

    uint16_t n = 20;
    wifi_ap_record_t recs[20];
    esp_wifi_scan_get_ap_records(&n, recs); /* sorted strongest first */

    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < count; j++) {
            dup |= strcmp(out[j].ssid, ssid) == 0;
        }
        if (dup) {
            continue;
        }
        strlcpy(out[count].ssid, ssid, sizeof(out[count].ssid));
        out[count].rssi = recs[i].rssi;
        out[count].open = recs[i].authmode == WIFI_AUTH_OPEN;
        count++;
    }
    return count;
}
