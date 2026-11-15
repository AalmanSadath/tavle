#pragma once

#include <stdbool.h>

/* Starts SNTP (pool.ntp.org). Call after the network stack is up. */
void timesync_start(void);

/* True once the clock has been set from the network. */
bool timesync_valid(void);

/* Stores a POSIX TZ string; applied by timesync_apply_tz on the UI task. */
void timesync_set_tz(const char *tz);
void timesync_apply_tz(void);
