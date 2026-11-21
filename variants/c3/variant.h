#pragma once

#include "driver/spi_master.h"

/* ESP32-C3, 4 MB flash, no PSRAM. Included through main/board.h. The wiring
 * comes from the hardware config at run time (main/hwcfg.h); this file holds
 * what depends on the chip and its memory. */
#define TAVLE_VARIANT_NAME  "c3"
#define TAVLE_CHIP_NAME     "ESP32-C3"

#define TAVLE_LCD_HOST      SPI2_HOST

/* One 20-line draw buffer: a second one costs 12.8 KB that TLS needs more
 * than the overlap of drawing and sending gains. */
#define TAVLE_LCD_BUF_LINES 20
#define TAVLE_LCD_DOUBLE_BUF 0

/* Limits: a moonraker source and one picture fill the heap. */
#define TAVLE_MAX_SOURCES   1
#define TAVLE_MAX_IMAGES    1
#define TAVLE_KEEP_RUNNING  0

/* Keep in step with "reserved" in variant.json, which tools/tavle.py checks.
 * Pins no hardware config may use: 11-17 are the SPI flash, 18 and 19 the
 * USB port the console runs on. */
#define TAVLE_RESERVED_PINS {11, 12, 13, 14, 15, 16, 17, 18, 19}

/* Hardware config used when none is saved: the reference wiring in the
 * README. SCK, MOSI and CS sit on the FSPI IOMUX pins so the bus can run at
 * 80 MHz without the GPIO matrix. The chip antenna on the reference board
 * only works with reduced transmit power (8.5 dBm). */
#define TAVLE_DEFAULT_HWCFG {                                              \
    .board = "ESP32-C3 reference",                                         \
    .display = "st7796", .spi_mhz = 40,                                    \
    .bgr = true, .invert = false, .mirror_x = true, .mirror_y = false,     \
    .sck = 6, .mosi = 7, .miso = 5, .cs = 10, .dc = 4, .rst = 3, .bl = 1,  \
    .button = 0, .tx_power_qdbm = 34,                                      \
}
