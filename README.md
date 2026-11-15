# tavle

A small always-on dashboard: an ESP32-C3 driving a 3.5" ST7796 TFT (320 x 480,
portrait) over SPI, powered over USB-C. Widgets are laid out on a 3 x 4 grid
from a JSON layout file.

Status: early. The device joins Wi-Fi, has a setup page, fetches its layout
from a URL, and shows live printer data from Moonraker. The `http-json` and
`mqtt` sources are not there yet.

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

## Layout

The layout is a JSON file at any URL the device can reach. Set the URL and
how often to check it on the setup page. Examples:

- A file in Klipper's config folder, served by Moonraker:
  `http://<printer>:7125/server/files/config/tavle.json`
  (add the device to `trusted_clients` in `moonraker.conf`)
- Any web server on your network: `http://<host>/tavle.json`
- A raw file on a Git host:
  `https://raw.githubusercontent.com/<user>/<repo>/main/tavle.json`

The device fetches the layout at startup, every few minutes as set, and when
you press reload layout. A layout that changed is shown and saved on the
device. If the URL cannot be reached, or the file is not a valid layout, the
device keeps showing the last good one, also after a restart; the setup page
shows the reason. Without a URL, or before the first successful fetch, a
built-in sample layout is shown.

### Printer data (Moonraker)

A `moonraker` source connects to Moonraker's websocket and subscribes to the
printer objects the widgets name:

```json
"sources": {
  "printer": { "type": "moonraker", "host": "10.1.0.231", "port": 7125 }
}
```

Use the port Moonraker answers on: 7125 by default, or the port of Mainsail or
Fluidd when they proxy it. Add the device to `trusted_clients` in
`moonraker.conf` and restart Moonraker; no API key is used.

A widget's `path` is the Klipper object, a dot, and the field, for example
`extruder.temperature`, `heater_bed.target`, `print_stats.state`,
`print_stats.print_duration`, `virtual_sdcard.progress` or
`heater_fan heatbreak_cooling_fan.speed`.

A graph adds a point every `period` seconds (default 10) and keeps 30 points.

Widgets show `--` until their first value, and are dimmed while their source
is offline.

### Clock

The clock gets the time from `pool.ntp.org`. Set the time zone on the setup
page as a POSIX TZ string, for example `AEST-10AEDT,M10.1.0,M4.1.0/3`.

Limits: 16 KB per layout file. HTTPS servers must have a certificate from a
widely used authority; self-signed certificates are refused.

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
