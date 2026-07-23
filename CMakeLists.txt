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
    src/input_events.c
    src/shtc3_thread.c
    src/display_thread.c
    src/device_ui.c
    src/chicago_18.c
    src/geneva_14.c
    src/geneva_9.c
    src/dseg_bold_italic_200_compressed.c
    src/material_design_40.c
    src/material_design_120.c
    src/llss_thread.c
    src/llss_storage.c
    src/llss_client.c
    src/llss_strip_cache.c
    src/press_feedback.c
    src/net_watchdog.c
    src/sys_watchdog.c
    src/ntp_sync.c
)

# Custom SSD16xx driver is hardware-only — SDL display takes its place on sim.
if(NOT BOARD MATCHES "native_sim")
    target_sources(app PRIVATE src/display_ssd16xx_800x480.c)
    target_sources_ifdef(CONFIG_LLSS_EPD_TEST_SHELL app PRIVATE src/epd_test_shell.c)
endif()

# Temporary deterministic frame pattern self-test.
target_sources_ifdef(CONFIG_LLSS_PATTERN_TEST app PRIVATE src/pattern_check.c)

# Temporary net/heap resource leak telemetry.
target_sources_ifdef(CONFIG_LLSS_NETMEM_DEBUG app PRIVATE src/netmem_debug.c)

# -----------------------------------------------------------------------------
# Selective speed optimization for the render path
#
# The rest of the image stays at -Os (CONFIG_SIZE_OPTIMIZATIONS). That is
# deliberate and load-bearing: this Espressif xtensa GCC miscompiles
# http_parser.c at -Og (Content-Length parses only its first digit -> -EBADMSG
# on every registration). See the note in debug.conf. Do not globally raise the
# optimization level to "fix" rendering.
#
# Instead only the hot render path is built for speed. Per-source and per-target
# options are emitted after the global flags, and GCC honours the last -O, so
# these override -Os for these files only.
#
# Measured hot spots (BTNTRACE, full server frame): LVGL rasterisation ~215 ms,
# our L8->plane conversion in the driver ~85 ms.
set(LLSS_RENDER_OPT_FLAGS -O2)

set_source_files_properties(
    src/display_thread.c
    src/press_feedback.c
    PROPERTIES COMPILE_OPTIONS "${LLSS_RENDER_OPT_FLAGS}"
)

# NOTE: display_ssd16xx_800x480.c is deliberately NOT in that list. -O2 makes
# its L8->plane conversion loop measurably SLOWER (2026-07-23, full 800x480
# frame: wr_us 85 ms at -Os vs 93 ms at -O2, three samples each, ranges do not
# overlap), while -O2 is a large win for LVGL (215 -> 145 ms). Per-file control
# lets us take both. Re-measure with `BTNTRACE ... wr_us=` before changing.

# LVGL itself is the larger share. target_compile_options() does NOT work here:
# Zephyr's global -Os arrives via the zephyr_interface INTERFACE library, which
# CMake emits *after* a target's own options, so -Os would win. Source-file
# properties are emitted last, so set them on LVGL's sources in the module's own
# directory scope (TARGET_DIRECTORY, CMake >= 3.18).
if(TARGET modules__lvgl)
    get_target_property(LLSS_LVGL_SRCS modules__lvgl SOURCES)
    if(LLSS_LVGL_SRCS)
        set_source_files_properties(
            ${LLSS_LVGL_SRCS}
            TARGET_DIRECTORY modules__lvgl
            PROPERTIES COMPILE_OPTIONS "${LLSS_RENDER_OPT_FLAGS}"
        )
    else()
        message(WARNING "modules__lvgl has no SOURCES — LVGL left at -Os")
    endif()
else()
    message(WARNING "modules__lvgl target not found — LVGL left at -Os")
endif()
