#pragma once

#include <stdbool.h>

#include "sdkconfig.h"

/* Board profile, picked at build time with idf.py -DBOARD=<board>. Pins and
 * display settings are macros in boards/<board>/board.h; the limits are also
 * in board_profile for code that reports them. */
#if CONFIG_TAVLE_BOARD_C3
#include "../boards/c3/board.h"
#elif CONFIG_TAVLE_BOARD_S3
#include "../boards/s3/board.h"
#else
#error "no board profile selected"
#endif

typedef struct {
    const char *name;
    int max_sources;    /* sources running at once */
    int max_images;     /* image widgets */
    bool keep_running;  /* default of "keep other screens running" */
} board_profile_t;

extern const board_profile_t board_profile;
