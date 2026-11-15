cmake_minimum_required(VERSION 3.16)

# The board is picked at configure time: idf.py -B build/<board> -DBOARD=<board> build
# Each board has its own sdkconfig (inside its build directory) and component lock.
set(TAVLE_BOARDS c3 s3)
set(BOARD c3 CACHE STRING "Board profile: one of ${TAVLE_BOARDS}")
if(NOT BOARD IN_LIST TAVLE_BOARDS)
    message(FATAL_ERROR "Unknown BOARD '${BOARD}'. Known boards: ${TAVLE_BOARDS}")
endif()

set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/boards/${BOARD}/sdkconfig.defaults")
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.${BOARD}.lock")
project(tavle)

string(TOUPPER "${BOARD}" board_upper)
if(NOT CONFIG_TAVLE_BOARD_${board_upper})
    message(FATAL_ERROR "sdkconfig in ${CMAKE_BINARY_DIR} is not for board ${BOARD}; "
                        "use a separate build directory per board (-B build/${BOARD})")
endif()
