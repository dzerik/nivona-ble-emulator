cmake_minimum_required(VERSION 3.16)

# --- Board profile selection -------------------------------------------
# Pick a board with: idf.py -DBOARD=<name> build
# Profiles live in boards/<name>/ and provide sdkconfig.board + partitions.csv
# + board.c. Default keeps the historical XIAO ESP32-C6 build.
if(NOT DEFINED BOARD)
    set(BOARD "xiao_c6" CACHE STRING "Target board profile")
endif()
set(_board_dir "${CMAKE_CURRENT_LIST_DIR}/boards/${BOARD}")
if(NOT EXISTS "${_board_dir}/sdkconfig.board")
    message(FATAL_ERROR
        "Unknown BOARD='${BOARD}'. Expected boards/${BOARD}/sdkconfig.board. "
        "Known: xiao_c6, xiao_s3, waveshare_c6_lcd_1_47")
endif()
message(STATUS "Building for BOARD='${BOARD}' (${_board_dir})")

# Chain the common defaults with the per-board fragment. The board fragment
# sets CONFIG_IDF_TARGET, flash size, partition CSV path, and feature flags.
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${_board_dir}/sdkconfig.board")

# BOARD is a CACHE variable, so main/CMakeLists.txt can read ${BOARD} directly
# for component selection — no extra variable needed.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nivona_emulator)
