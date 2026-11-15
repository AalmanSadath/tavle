#include "display_driver.h"

#include <inttypes.h>
#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ST7796, 320 x 480 portrait, four-wire SPI. Pins, SPI host and clock, and
 * the panel options (colour order, invert, mirror) come from the board
 * header. */

static const char *TAG = "st7796";

#define WIDTH  320
#define HEIGHT 480

typedef struct {
    uint8_t id4[4];       /* raw 0xD3 response, starts with one dummy bit */
    uint8_t rddid[4];     /* raw 0x04 response, starts with one dummy bit */
    uint32_t id4_value;   /* 24-bit ID4 with the dummy bit removed: 0x007796 */
    uint32_t rddid_value; /* 24-bit RDDID with the dummy bit removed */
    bool is_st7796;
} st7796_id_t;

static size_t s_max_transfer;

static esp_err_t bus_init(bool with_miso)
{
    const spi_bus_config_t bus = {
        .sclk_io_num = TAVLE_PIN_LCD_SCK,
        .mosi_io_num = TAVLE_PIN_LCD_MOSI,
        .miso_io_num = with_miso ? TAVLE_PIN_LCD_MISO : -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = s_max_transfer,
    };
    return spi_bus_initialize(TAVLE_LCD_HOST, &bus, SPI_DMA_CH_AUTO);
}

static void hw_reset(void)
{
    gpio_set_level(TAVLE_PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TAVLE_PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

/* One read command: command byte with DC low, then n bytes clocked in with
 * DC high, CS held low across both phases. */
static esp_err_t read_reg(spi_device_handle_t dev, uint8_t cmd, uint8_t *out, size_t n)
{
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(dev, portMAX_DELAY), TAG, "acquire");

    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_CS_KEEP_ACTIVE,
        .length = 8,
        .tx_data = {cmd},
    };
    gpio_set_level(TAVLE_PIN_LCD_DC, 0);
    esp_err_t err = spi_device_polling_transmit(dev, &t);

    if (err == ESP_OK) {
        memset(&t, 0, sizeof(t));
        t.flags = SPI_TRANS_USE_RXDATA;
        t.length = 8 * n;
        t.rxlength = 8 * n;
        gpio_set_level(TAVLE_PIN_LCD_DC, 1);
        err = spi_device_polling_transmit(dev, &t);
        memcpy(out, t.rx_data, n);
    }

    spi_device_release_bus(dev);
    return err;
}

static uint32_t unshift_dummy_bit(const uint8_t b[4])
{
    uint32_t raw = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    return (raw << 1) >> 8;
}

static esp_err_t read_id(st7796_id_t *id)
{
    const gpio_config_t out = {
        .pin_bit_mask = BIT64(TAVLE_PIN_LCD_DC) | BIT64(TAVLE_PIN_LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "gpio");
    hw_reset();

    ESP_RETURN_ON_ERROR(bus_init(true), TAG, "bus with miso");

    const spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = TAVLE_PIN_LCD_CS,
        .queue_size = 1,
    };
    spi_device_handle_t dev;
    ESP_RETURN_ON_ERROR(spi_bus_add_device(TAVLE_LCD_HOST, &devcfg, &dev), TAG, "add dev");

    esp_err_t err = read_reg(dev, 0xD3, id->id4, 4);
    if (err == ESP_OK) {
        err = read_reg(dev, 0x04, id->rddid, 4);
    }

    spi_bus_remove_device(dev);
    spi_bus_free(TAVLE_LCD_HOST);

    /* In four-wire SPI the panel sends one dummy bit before the data, so
     * the 24-bit value sits one bit to the right inside the four bytes. */
    id->id4_value = unshift_dummy_bit(id->id4);
    id->rddid_value = unshift_dummy_bit(id->rddid);
    id->is_st7796 = id->id4_value == 0x007796;
    ESP_LOGI(TAG, "ID4 (0xD3): raw %02x %02x %02x %02x, id %06" PRIx32 " -> %s",
             id->id4[0], id->id4[1], id->id4[2], id->id4[3], id->id4_value,
             id->is_st7796 ? "ST7796" : "not ST7796 (or MISO not connected)");
    ESP_LOGI(TAG, "RDDID (0x04): raw %02x %02x %02x %02x, id %06" PRIx32,
             id->rddid[0], id->rddid[1], id->rddid[2], id->rddid[3], id->rddid_value);
    return err;
}

static esp_err_t st7796_init(int buf_lines, display_panel_t *out)
{
    s_max_transfer = (size_t)WIDTH * buf_lines * sizeof(uint16_t);
    st7796_id_t id = {0};
    if (read_id(&id) != ESP_OK) {
        ESP_LOGW(TAG, "ID read failed, continuing");
    }

    const gpio_config_t bl = {
        .pin_bit_mask = BIT64(TAVLE_PIN_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "bl gpio");
    gpio_set_level(TAVLE_PIN_LCD_BL, 0);

    /* Pixel data only goes out, so the running bus has no MISO and every
     * remaining pin sits on IOMUX. */
    ESP_RETURN_ON_ERROR(bus_init(false), TAG, "bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = TAVLE_PIN_LCD_DC,
        .cs_gpio_num = TAVLE_PIN_LCD_CS,
        .pclk_hz = CONFIG_TAVLE_LCD_SPI_MHZ * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TAVLE_LCD_HOST,
                                                 &io_cfg, &out->io), TAG, "panel io");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = TAVLE_PIN_LCD_RST,
        .rgb_ele_order = TAVLE_LCD_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(out->io, &panel_cfg, &out->panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(out->panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(out->panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(out->panel, TAVLE_LCD_INVERT), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(out->panel, TAVLE_LCD_MIRROR_X, TAVLE_LCD_MIRROR_Y),
                        TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(out->panel, true), TAG, "on");

    out->width = WIDTH;
    out->height = HEIGHT;
    out->mirror_x = TAVLE_LCD_MIRROR_X;
    out->mirror_y = TAVLE_LCD_MIRROR_Y;
    out->swap_xy = false;
    out->swap_bytes = true; /* the panel takes RGB565 high byte first */
    return ESP_OK;
}

static void st7796_backlight(bool on)
{
    gpio_set_level(TAVLE_PIN_LCD_BL, on);
}

const display_driver_t display_st7796 = {
    .name = "ST7796",
    .init = st7796_init,
    .backlight = st7796_backlight,
};
