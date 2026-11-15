#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

/* Display: ST7796, 320 x 480, four-wire SPI. SCK, MOSI and CS sit on the
 * ESP32-C3 FSPI IOMUX pins so the bus can run at 80 MHz without the GPIO
 * matrix. MISO is only used for the ID read at boot. */
#define TAVLE_LCD_HOST      SPI2_HOST
#define TAVLE_PIN_LCD_SCK   GPIO_NUM_6
#define TAVLE_PIN_LCD_MOSI  GPIO_NUM_7
#define TAVLE_PIN_LCD_MISO  GPIO_NUM_5
#define TAVLE_PIN_LCD_CS    GPIO_NUM_10
#define TAVLE_PIN_LCD_DC    GPIO_NUM_4
#define TAVLE_PIN_LCD_RST   GPIO_NUM_3
#define TAVLE_PIN_LCD_BL    GPIO_NUM_1

/* Push button to GND, internal pull-up. */
#define TAVLE_PIN_BUTTON    GPIO_NUM_0

#define TAVLE_LCD_H_RES     320
#define TAVLE_LCD_V_RES     480
#define TAVLE_LCD_BUF_LINES 40

/* Panel options, adjusted after first look at the screen. */
#define TAVLE_LCD_BGR       1
#define TAVLE_LCD_INVERT    0
#define TAVLE_LCD_MIRROR_X  1
#define TAVLE_LCD_MIRROR_Y  0
