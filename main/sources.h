#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "layout.h"

#define SRC_PATH_MAX 64
#define SRC_TEXT_MAX 320 /* longest text value; a picture address is one */

typedef enum {
    SRC_STARTING,
    SRC_ONLINE,
    SRC_OFFLINE,
    SRC_NOT_BUILT,
    SRC_STOPPED,
} src_state_t;

/* A source type built into the firmware. start() copies what it needs from
 * config and paths; both are only valid during the call. Reports may come
 * from any task until stop() returns. */
typedef struct {
    const char *type;
    void *(*start)(int index, const cJSON *config, const char *const *paths, int npaths);
    void (*stop)(void *ctx);
} source_driver_t;

typedef struct {
    char name[24];
    char type[12];
    src_state_t state;
    char detail[64];
} source_status_t;

/* UI task: stops every running driver and takes layout's sources, all
 * stopped. layout must stay valid until the next sources_load. */
void sources_load(const layout_t *layout);

/* UI task: runs exactly the sources marked in want (bit i = sources[i]).
 * Sources that stay wanted keep running untouched. */
void sources_apply(uint16_t want);
void sources_stop_all(void);

/* True when source index has a running driver. */
bool sources_running(int index);

/* Changes whenever source index starts or stops, so values queued by an
 * older driver of that source are dropped. */
unsigned sources_generation(int index);

/* Driver side. */
void sources_report_number(int index, const char *path, double v);
void sources_report_text(int index, const char *path, const char *s);
void sources_report_state(int index, src_state_t state, const char *detail);

int sources_get_status(source_status_t *out, int max);
const char *src_state_name(src_state_t state);
