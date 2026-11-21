#include "button.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "fetch.h"
#include "hwcfg.h"
#include "net.h"
#include "settings.h"
#include "store.h"
#include "timesync.h"
#include "ui.h"
#include "web.h"

static const char *TAG = "tavle";

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "heap %s: internal free %u, min free %u, largest DMA block %u, PSRAM free %u of %u",
             when, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
}

/* A freshly uploaded image boots as pending. It is kept only once the
 * display and the setup page are up, so an update that breaks either is
 * rolled back by the bootloader on the next reset. */
static void on_net_status(const net_status_t *status)
{
    ui_net_status(status);
    if (status->state == NET_CONNECTED) {
        fetch_network_up();
    }
}

static void confirm_running_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware in %s confirmed", running->label);
    }
}

void app_main(void)
{
    log_heap("at boot");
    ESP_ERROR_CHECK(settings_init());
    hwcfg_load(); /* pins and display, before anything uses them */

    /* Display first: its DMA buffers need large free blocks, which Wi-Fi
     * would fragment. */
    lv_display_t *disp;
    ESP_ERROR_CHECK(display_init(&disp));
    log_heap("after display init");

    /* The network stack must exist before the UI starts: a saved layout can
     * start data sources, whose sockets need lwIP even before Wi-Fi joins. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    store_init(); /* without it the device still runs on the built-in layout */
    ESP_ERROR_CHECK(fetch_start());
    ESP_ERROR_CHECK(ui_start(disp));
    ESP_ERROR_CHECK(button_start());
    settings_t s;
    settings_get(&s);
    timesync_set_tz(s.tz);
    ESP_ERROR_CHECK(net_start(on_net_status));
    timesync_start();
    ESP_ERROR_CHECK(web_start());
    log_heap("after wi-fi and web");

    confirm_running_image();
}
