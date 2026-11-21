cmake_minimum_required(VERSION 3.16)

# The firmware variant (chip and memory) is picked at configure time:
#   idf.py -B build/<variant> -DVARIANT=<variant> build
# Each variant has its own sdkconfig (inside its build directory) and
# component lock. Pins and the display are not part of the build: they come
# from the hardware config the device reads at boot.
set(TAVLE_VARIANTS c3 s3-n16r8)
set(VARIANT c3 CACHE STRING "Firmware variant: one of ${TAVLE_VARIANTS}")
if(NOT VARIANT IN_LIST TAVLE_VARIANTS)
    message(FATAL_ERROR "Unknown VARIANT '${VARIANT}'. Known variants: ${TAVLE_VARIANTS}")
endif()

set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/variants/${VARIANT}/sdkconfig.defaults")
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.${VARIANT}.lock")
project(tavle)

string(TOUPPER "${VARIANT}" variant_upper)
string(REPLACE "-" "_" variant_upper "${variant_upper}")
if(NOT CONFIG_TAVLE_VARIANT_${variant_upper})
    message(FATAL_ERROR "sdkconfig in ${CMAKE_BINARY_DIR} is not for variant ${VARIANT}; "
                        "use a separate build directory per variant (-B build/${VARIANT})")
endif()
