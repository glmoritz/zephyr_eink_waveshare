cmake_minimum_required(VERSION 3.20.0)

set(NO_BUILD_TYPE_WARNING ON CACHE BOOL "Suppress Zephyr build-type warning for Kconfig-driven optimization levels" FORCE)

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# Pick the host-stub provisioning module on native_sim, the real ESP32
# WiFi captive-portal module everywhere else.
if(BOARD MATCHES "native_sim")
    list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/modules/wifi_prov_sim)
else()
    list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/modules/wifi_prov)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hello_eink)

target_sources(app PRIVATE
    src/main.c
    src/system_flags.c
    src/display_thread.c
    src/llss_thread.c
    src/llss_storage.c
    src/llss_client.c
    src/ntp_sync.c
)

# Custom SSD16xx driver is hardware-only — SDL display takes its place on sim.
if(NOT BOARD MATCHES "native_sim")
    target_sources(app PRIVATE src/display_ssd16xx_800x480.c)
endif()
