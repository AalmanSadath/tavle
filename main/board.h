#pragma once

#include <stdbool.h>

#include "sdkconfig.h"

/* Firmware variant, picked at build time with idf.py -DVARIANT=<variant>.
 * Memory settings and limits are macros in variants/<variant>/variant.h and
 * also in board_profile for code that reports them. Pins and the display
 * are in the hardware config (hwcfg.h). */
#if CONFIG_TAVLE_VARIANT_C3
#include "../variants/c3/variant.h"
#elif CONFIG_TAVLE_VARIANT_S3_N16R8
#include "../variants/s3-n16r8/variant.h"
#else
#error "no firmware variant selected"
#endif

typedef struct {
    const char *name;   /* chip, as shown on the setup page */
    const char *variant;
    int max_sources;    /* sources running at once */
    int max_images;     /* image widgets */
    bool keep_running;  /* default of "keep other screens running" */
} board_profile_t;

extern const board_profile_t board_profile;
