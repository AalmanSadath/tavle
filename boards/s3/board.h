#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

/* ESP32-S3-WROOM-1 N16R8: 16 MB flash, 8 MB octal PSRAM. Included through
 * main/board.h. GPIO 26-32 (flash) and 35-37 (PSRAM) are taken, 19-20 are
 * USB, 3, 45 and 46 are strapping pins: none of them is used. */
#define TAVLE_BOARD_NAME    "ESP32-S3"

/* Display wiring, for the four-wire SPI display picked with
 * CONFIG_TAVLE_DISPLAY. SCK, MOSI, MISO and CS sit on the FSPI IOMUX pins
 * (12, 11, 13, 10) so the bus can run at 80 MHz without the GPIO matrix.
 * MISO is only used for the ID read at boot. */
#define TAVLE_LCD_HOST      SPI2_HOST
#define TAVLE_PIN_LCD_SCK   GPIO_NUM_12
#define TAVLE_PIN_LCD_MOSI  GPIO_NUM_11
#define TAVLE_PIN_LCD_MISO  GPIO_NUM_13
#define TAVLE_PIN_LCD_CS    GPIO_NUM_10
#define TAVLE_PIN_LCD_DC    GPIO_NUM_9
#define TAVLE_PIN_LCD_RST   GPIO_NUM_14
#define TAVLE_PIN_LCD_BL    GPIO_NUM_21

/* Push button to GND, internal pull-up. GPIO 0 is the BOOT button. */
#define TAVLE_PIN_BUTTON    GPIO_NUM_0

/* Two 40-line draw buffers in internal DMA memory: drawing overlaps sending. */
#define TAVLE_LCD_BUF_LINES 40
#define TAVLE_LCD_DOUBLE_BUF 1

/* Options of the display module: colour order, invert, mirror. */
#define TAVLE_LCD_BGR       1
#define TAVLE_LCD_INVERT    0
#define TAVLE_LCD_MIRROR_X  1
#define TAVLE_LCD_MIRROR_Y  0

/* Limits: a starting point, to be measured and raised. */
#define TAVLE_MAX_SOURCES   4
#define TAVLE_MAX_IMAGES    4
#define TAVLE_KEEP_RUNNING  1
