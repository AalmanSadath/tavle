#pragma once

#include "driver/spi_master.h"

/* ESP32-S3-WROOM-1 N16R8: 16 MB flash, 8 MB octal PSRAM. Included through
 * main/board.h. The wiring comes from the hardware config at run time
 * (main/hwcfg.h); this file holds what depends on the chip and its memory. */
#define TAVLE_VARIANT_NAME  "s3-n16r8"
#define TAVLE_CHIP_NAME     "ESP32-S3"

#define TAVLE_LCD_HOST      SPI2_HOST

/* Two 20-line draw buffers in internal DMA memory: drawing overlaps sending.
 * 40 lines cost 25 KB more internal RAM than four sources can spare. */
#define TAVLE_LCD_BUF_LINES 20
#define TAVLE_LCD_DOUBLE_BUF 1

/* Limits, measured: with four moonraker sources and four pictures about 69 KB
 * of internal RAM stays free; each further source costs about 9 KB (its
 * websocket task stack is always internal), so six leave 51 KB and eight
 * 21 KB. */
#define TAVLE_MAX_SOURCES   4
#define TAVLE_MAX_IMAGES    4
#define TAVLE_KEEP_RUNNING  1

/* Keep in step with "reserved" in variant.json, which tools/tavle.py checks.
 * Pins no hardware config may use: 19 and 20 are USB, 26-32 the flash and
 * 33-37 the octal PSRAM. */
#define TAVLE_RESERVED_PINS {19, 20, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37}

/* Hardware config used when none is saved: the reference wiring in the
 * README. SCK, MOSI, MISO and CS sit on the FSPI IOMUX pins (12, 11, 13, 10)
 * so the bus can run at 80 MHz without the GPIO matrix. GPIO 0 is the BOOT
 * button. */
#define TAVLE_DEFAULT_HWCFG {                                                \
    .board = "ESP32-S3 N16R8 reference",                                     \
    .display = "st7796", .spi_mhz = 40,                                      \
    .bgr = true, .invert = false, .mirror_x = true, .mirror_y = false,       \
    .sck = 12, .mosi = 11, .miso = 13, .cs = 10, .dc = 9, .rst = 14, .bl = 21, \
    .button = 0, .tx_power_qdbm = 0,                                         \
}
