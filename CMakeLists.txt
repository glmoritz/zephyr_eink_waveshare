cmake_minimum_required(VERSION 3.20.0)

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/modules/wifi_prov)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hello_eink)

target_sources(app PRIVATE
    src/main.c
    src/display_ssd16xx_800x480.c
    src/llss_storage.c
    src/llss_client.c
    src/ntp_sync.c
)
