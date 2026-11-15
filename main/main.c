#include <inttypes.h>
#include <string.h>

#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "layout.h"
#include "widgets.h"

static const char *TAG = "tavle";

extern const char layout_json_start[] asm("_binary_layout_json_start");
extern const char layout_json_end[] asm("_binary_layout_json_end");

static layout_t s_layout;
static screen_view_t s_views[LAYOUT_MAX_SCREENS];

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "heap %s: free %u, min free %u, largest DMA block %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

#if CONFIG_TAVLE_BENCH
/* Times one LVGL refresh from first draw call until the last pixel is on
 * the panel. Caller holds the LVGL lock. */
static int64_t timed_refresh(lv_display_t *disp)
{
    int64_t t0 = esp_timer_get_time();
    lv_refr_now(disp);
    display_wait_idle();
    return esp_timer_get_time() - t0;
}

static void bench_task(void *arg)
{
    lv_display_t *disp = arg;
    screen_view_t *view = &s_views[0];
    const int full_runs = 20, update_runs = 100;
    int64_t total = 0, worst = 0;

    lvgl_port_lock(0);
    for (int i = 0; i < full_runs; i++) {
        lv_obj_invalidate(view->screen);
        int64_t us = timed_refresh(disp);
        total += us;
        worst = us > worst ? us : worst;
    }
    lvgl_port_unlock();
    ESP_LOGI(TAG, "bench full screen @ %d MHz: avg %.1f ms (%.1f fps), worst %.1f ms",
             CONFIG_TAVLE_LCD_SPI_MHZ, total / 1000.0 / full_runs,
             1e6 * full_runs / total, worst / 1000.0);

    total = 0;
    worst = 0;
    lvgl_port_lock(0);
    for (int i = 0; i < update_runs; i++) {
        widgets_bench_step(view, i);
        int64_t us = timed_refresh(disp);
        total += us;
        worst = us > worst ? us : worst;
    }
    lvgl_port_unlock();
    ESP_LOGI(TAG, "bench all widgets changing @ %d MHz: avg %.1f ms (%.1f fps), worst %.1f ms",
             CONFIG_TAVLE_LCD_SPI_MHZ, total / 1000.0 / update_runs,
             1e6 * update_runs / total, worst / 1000.0);

    lvgl_port_lock(0);
    widgets_fill_placeholders(view);
    lvgl_port_unlock();
    log_heap("after bench");
    vTaskDelete(NULL);
}
#endif

void app_main(void)
{
    log_heap("at boot");

    lv_display_t *disp;
    display_id_t id;
    ESP_ERROR_CHECK(display_init(&id, &disp));
    log_heap("after display init");

    size_t len = layout_json_end - layout_json_start;
    while (len && layout_json_start[len - 1] == '\0') {
        len--; /* EMBED_TXTFILES appends a terminator */
    }
    if (!layout_parse(layout_json_start, len, &s_layout)) {
        ESP_LOGE(TAG, "embedded layout has no usable screen");
        return;
    }

    lvgl_port_lock(0);
    widgets_build(disp, &s_layout, s_views);
    for (int i = 0; i < s_layout.screen_count; i++) {
        widgets_fill_placeholders(&s_views[i]);
    }
    lv_screen_load(s_views[0].screen);
    lvgl_port_unlock();
    log_heap("after widgets");

#if CONFIG_TAVLE_BENCH
    /* Rendering runs on the calling task's stack, so the benchmark gets
     * its own task instead of the small main task. */
    vTaskDelay(pdMS_TO_TICKS(500));
    xTaskCreate(bench_task, "bench", 8192, disp, 4, NULL);
#endif
}
