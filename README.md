# tavle

A small always-on dashboard: an ESP32-C3 driving a 3.5" ST7796 TFT (320 x 480,
portrait) over SPI, powered over USB-C. Widgets are laid out on a 3 x 4 grid
from a JSON layout file.

Status: early bring-up. The firmware draws a built-in sample layout with
placeholder values. Wi-Fi, data sources and the setup page are not there yet.

## Wiring

| Display pin | ESP32-C3 GPIO |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCL | 6 |
| SDA | 7 |
| SDA-O | 5 |
| CS | 10 |
| DC | 4 |
| RST | 3 |
| BL | 1 |

Button: GPIO 0 to GND.

The display's IM0, IM1 and IM2 jumpers must be set for four-wire SPI (1, 1, 1).

## Build and flash

Requires ESP-IDF v5.4.

```sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```
