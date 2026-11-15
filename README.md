# tavle

A small always-on dashboard: an ESP32-C3 driving a 3.5" ST7796 TFT (320 x 480,
portrait) over SPI, powered over USB-C. Widgets are laid out on a 3 x 4 grid
from a JSON layout file.

Status: early. The device joins Wi-Fi and has a setup page, and draws a
built-in sample layout with placeholder values. Fetching layouts from a URL
and live data sources are not there yet.

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

## First setup

1. Power the device. With no saved Wi-Fi it opens its own network, shown on
   the screen as `tavle-xxxx` with a password and a QR code.
2. Scan the QR code with a phone (or join the network by hand). The setup
   page opens by itself; if it does not, go to `http://192.168.4.1`.
3. Pick your Wi-Fi network, enter its password, and press save. An admin
   password is optional.
4. The device joins your network and shows its address for a few seconds.

If the saved network cannot be reached for a minute, the device opens its
setup network again, so Wi-Fi can be changed without a cable.

## Setup page

On your network the page is at `http://tavle.local/` (or the address shown at
startup). It has the Wi-Fi settings, the layout URL, how often to check it,
a layout reload button, a reboot button, and firmware upload.

Without an admin password, anyone on your network can open the page, change
settings and upload firmware. With one, the page asks for it. The page uses
plain HTTP, so someone watching traffic on the same network could still read
the password.

## Firmware update

Over USB with `idf.py flash`, or on the setup page: choose `build/tavle.bin`
and press upload firmware. The device checks the file, restarts into it, and
goes back to the previous firmware if the new one fails to start.

## Wi-Fi range

Some small ESP32-C3 boards only work with reduced Wi-Fi transmit power, so
it is capped at 8.5 dBm by default. If the device cannot reach your router,
raise `TAVLE_WIFI_TX_POWER_QDBM` in `idf.py menuconfig` (tavle menu).

## Build and flash

Requires ESP-IDF v5.4.

```sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```
