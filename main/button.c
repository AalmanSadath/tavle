#include "button.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "ui.h"

static const char *TAG = "button";

#define POLL_MS     20
#define DEBOUNCE    3    /* polls the level must hold before it counts */
#define SHORT_MAX_MS 1000
#define REBOOT_MS   2000

static void poll(void *arg)
{
    static bool down;     /* debounced state */
    static int same;      /* polls the raw level has differed from it */
    static int held_ms;

    bool raw = gpio_get_level(TAVLE_PIN_BUTTON) == 0;
    if (raw != down) {
        if (++same < DEBOUNCE) {
            return;
        }
        down = raw;
        same = 0;
        if (!down && held_ms < SHORT_MAX_MS) {
            ui_next_screen();
        }
        held_ms = 0;
        return;
    }
    same = 0;
    if (down) {
        held_ms += POLL_MS;
        if (held_ms >= REBOOT_MS) {
            ESP_LOGI(TAG, "held, rebooting");
            esp_restart();
        }
    }
}

esp_err_t button_start(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(TAVLE_PIN_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    const esp_timer_create_args_t args = {.callback = poll, .name = "button"};
    esp_timer_handle_t t;
    err = esp_timer_create(&args, &t);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(t, POLL_MS * 1000);
    }
    return err;
}
