#include "display.h"

#include <string.h>

#include "board.h"
#include "display_driver.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "hwcfg.h"

static const char *TAG = "display";

/* Every driver is in every build; the hardware config picks one by name. */
static const struct {
    const char *name;
    const display_driver_t *driver;
} s_drivers[] = {
    {"st7796", &display_st7796},
};

static const display_driver_t *find(const char *name)
{
    for (size_t i = 0; i < sizeof(s_drivers) / sizeof(s_drivers[0]); i++) {
        if (strcmp(s_drivers[i].name, name) == 0) {
            return s_drivers[i].driver;
        }
    }
    return NULL;
}

bool display_known(const char *driver)
{
    return find(driver) != NULL;
}

static display_panel_t s_panel;
static lv_display_t *s_disp;

#define MARK_SIZE 10
#define MARK_GAP  4
static uint16_t s_mark[MARK_SIZE * MARK_SIZE]; /* internal RAM, DMA capable */

esp_err_t display_init(lv_display_t **disp_out)
{
    const hwcfg_t *hw = hwcfg();
    const display_driver_t *drv = find(hw->display);
    ESP_RETURN_ON_FALSE(drv, ESP_ERR_NOT_FOUND, TAG, "no driver %s", hw->display);
    ESP_RETURN_ON_ERROR(drv->init(hw, TAVLE_LCD_BUF_LINES, &s_panel), TAG, "%s", drv->name);

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_panel.io,
        .panel_handle = s_panel.panel,
        .buffer_size = s_panel.width * TAVLE_LCD_BUF_LINES,
        .double_buffer = TAVLE_LCD_DOUBLE_BUF,
        .hres = s_panel.width,
        .vres = s_panel.height,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = s_panel.swap_xy,
            .mirror_x = s_panel.mirror_x,
            .mirror_y = s_panel.mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = s_panel.swap_bytes,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "add disp");

    drv->backlight(true);
    s_disp = disp;
    *disp_out = disp;
    ESP_LOGI(TAG, "%s %dx%d on %s at %d MHz, %s %d-line buffer%s", drv->name,
             s_panel.width, s_panel.height, hw->board, hw->spi_mhz,
             TAVLE_LCD_DOUBLE_BUF ? "two" : "one", TAVLE_LCD_BUF_LINES,
             TAVLE_LCD_DOUBLE_BUF ? "s" : "");
    return ESP_OK;
}

int display_width(void)
{
    return s_panel.width;
}

int display_height(void)
{
    return s_panel.height;
}

void display_wait_idle(void)
{
    /* A parameter write on the panel IO waits for all queued colour
     * transfers to finish before it goes out. NOP (0x00) changes nothing. */
    esp_lcd_panel_io_tx_param(s_panel.io, 0x00, NULL, 0);
}

void display_freeze(bool freeze)
{
    lv_timer_t *t = lv_display_get_refr_timer(s_disp);
    if (freeze) {
        lv_timer_pause(t);
    } else {
        lv_timer_resume(t);
        lv_obj_invalidate(lv_screen_active()); /* paint over the busy mark */
    }
}

void display_draw_mark(void)
{
    /* 0x8e8e93 is 0x8872 in RGB565 */
    const uint16_t grey = s_panel.swap_bytes ? 0x7288 : 0x8872;
    for (int i = 0; i < MARK_SIZE * MARK_SIZE; i++) {
        s_mark[i] = grey;
    }
    int x = s_panel.width - MARK_GAP - MARK_SIZE;
    esp_lcd_panel_draw_bitmap(s_panel.panel, x, MARK_GAP, x + MARK_SIZE, MARK_GAP + MARK_SIZE,
                              s_mark);
}
