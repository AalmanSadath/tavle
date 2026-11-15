#pragma once

#include "sources.h"

/* Moonraker websocket with printer object subscriptions. Layout source:
 * {"type": "moonraker", "host": "<printer>", "port": 7125} */
extern const source_driver_t src_moonraker_driver;
