# tavle

A small always-on dashboard: an ESP32 board driving a 3.5" ST7796 TFT (320 x 480,
portrait) over SPI, powered over USB-C. Supported boards: ESP32-C3 and
ESP32-S3-WROOM-1 N16R8 (16 MB flash, 8 MB PSRAM). Widgets are laid out on a 3 x 4 grid
from a JSON layout file.

Status: early. The device joins Wi-Fi, has a setup page, fetches its layout
from a URL, and shows live printer data from Moonraker. The `http-json` and
`mqtt` sources are not there yet.

## Wiring

| Display pin | ESP32-C3 GPIO | ESP32-S3 GPIO |
|---|---|---|
| VCC | 3V3 | 3V3 |
| GND | GND | GND |
| SCL | 6 | 12 |
| SDA | 7 | 11 |
| SDA-O | 5 | 13 |
| CS | 10 | 10 |
| DC | 4 | 9 |
| RST | 3 | 14 |
| BL | 1 | 21 |

Button: GPIO 0 to GND on both boards (on the S3 this is the BOOT button).
Press it for the next screen; hold it for two seconds to restart.

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
whether to keep other screens running (see Screens), a layout reload button, a
next screen button, a reboot button, and firmware upload.

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

### Screens

A layout can have several screens; the button or the next screen button on
the setup page moves to the next one. Each board runs a limited number of
sources at once (see Build and flash). A screen may use at most that many
sources and image widgets; a layout with a screen over the limit is refused
and the setup page shows why. Widgets sharing a source count once.

Setting "keep other screens running":

- off: only the screen on show runs its sources. Switching stops them and
  starts the next screen's; graphs start empty. While the new screen waits for
  its first data (at most 3 seconds) the old one stays up with a small grey
  mark in the top-right corner. Pressing again during the wait moves on to
  the following screen.
- on: screens viewed recently keep running in the background as far as the
  board's limits allow, so switching back to them is instant.
- auto: the layout's `"keep_running": true` or `false` if it has one, else the
  board default (off on the ESP32-C3, on on the ESP32-S3).

```json
{
  "keep_running": false,
  "sources": { ... },
  "screens": [ ... ]
}
```

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
`heater_fan heatbreak_cooling_fan.speed`. Some values are computed by the
device, the same way Mainsail shows them:

| Path | Value |
|---|---|
| `tavle.progress` | progress through the gcode between the slicer's start and end markers (0 to 1) |
| `tavle.slicer_remaining` | the slicer's estimate for the file minus the time printed |
| `tavle.klipper_remaining` | the mean of the next two, Mainsail's default estimate |
| `tavle.file_remaining` | the time printed scaled by `tavle.progress`; settles as the print goes on |
| `tavle.filament_remaining` | the time printed scaled by filament used out of the file's total |
| `tavle.layer` | current layer and layer count, as `12/67` |
| `tavle.filename` | file name without folder and `.gcode`; a slicer suffix like `_PETG_1h18m` is dropped |
| `tavle.thumbnail` | address of the file's thumbnail, for an `image` widget |

The remaining times and the layer show `--` when nothing is printing. Use
`"format": "duration"` to show seconds as `1h 23m`, or `23:05` under an hour.
A value too wide for its cell is drawn smaller; `12/340` that still does not
fit shows only `12`.

A progress bar can show a line of text under its label, such as the file name:

```json
{ "type": "progress", "col": 0, "row": 0, "colspan": 2, "label": "Progress",
  "source": "printer", "path": "tavle.progress", "text_path": "tavle.filename" }
```

An `image` widget shows a PNG whose `http://` address is the widget's value,
scaled to fit the cell:

```json
{ "type": "image", "col": 2, "row": 0, "source": "printer", "path": "tavle.thumbnail" }
```

PNG pictures must be at most 1024 pixels wide and high, 8 bits per channel,
and not interlaced. Slicer thumbnails are; a thumbnail between 100 and 1024
pixels wide is picked. Decoding needs a free 32 KB block of memory. If there
is none, the picture is tried again after 20 seconds.

A graph can draw up to three values of one source:

```json
{ "type": "graph", "col": 0, "row": 1, "colspan": 3, "rowspan": 2,
  "source": "printer", "paths": ["extruder.temperature", "heater_bed.temperature"],
  "labels": ["nozzle", "bed"], "min": 0, "max": 260, "period": 10 }
```

`period` is the seconds between points; the graph keeps 30 points.

Widgets show `--` until their first value, and are dimmed while their source
is offline.

### Clock

The clock gets the time from `pool.ntp.org`. Set the time zone on the setup
page as a POSIX TZ string, for example `AEST-10AEDT,M10.1.0,M4.1.0/3`.

Limits: 16 KB per layout file. HTTPS servers must have a certificate from a
widely used authority; self-signed certificates are refused.

## Firmware update

Over USB with `idf.py flash`, or on the setup page: choose
`build/<board>/tavle.bin` (for example `build/c3/tavle.bin`)
and press upload firmware. The device checks the file, restarts into it, and
goes back to the previous firmware if the new one fails to start.

## Wi-Fi range

Some small ESP32-C3 boards only work with reduced Wi-Fi transmit power, so
the C3 build caps it at 8.5 dBm by default. If the device cannot reach your router,
raise `TAVLE_WIFI_TX_POWER_QDBM` in `idf.py menuconfig` (tavle menu).

## Build and flash

Requires ESP-IDF v5.4.

Pick the board with `-DBOARD=` and give each board its own build directory:

```sh
idf.py -B build/c3 -DBOARD=c3 build
idf.py -B build/c3 -DBOARD=c3 -p /dev/ttyACM0 flash monitor

idf.py -B build/s3 -DBOARD=s3 build
```

Each board has limits that fit its memory; the setup page shows them. They
apply per screen, see Screens.

| | ESP32-C3 | ESP32-S3 |
|---|---|---|
| sources running at once | 1 | 4 |
| image widgets | 1 | 4 |
