#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "net.h"
#include "settings.h"
#include "ui.h"
#include "web.h"

static const char *TAG = "tavle";

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "heap %s: free %u, min free %u, largest DMA block %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

/* A freshly uploaded image boots as pending. It is kept only once the
 * display and the setup page are up, so an update that breaks either is
 * rolled back by the bootloader on the next reset. */
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

    /* Display first: its DMA buffers need large free blocks, which Wi-Fi
     * would fragment. */
    lv_display_t *disp;
    display_id_t id;
    ESP_ERROR_CHECK(display_init(&id, &disp));
    log_heap("after display init");

    ESP_ERROR_CHECK(ui_start(disp));
    ESP_ERROR_CHECK(net_start(ui_net_status));
    ESP_ERROR_CHECK(web_start(ui_request_reload));
    log_heap("after wi-fi and web");

    confirm_running_image();
}
