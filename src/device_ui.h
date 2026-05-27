#ifndef DEVICE_UI_H_
#define DEVICE_UI_H_

#include <stdbool.h>
#include <lvgl.h>

#include "input_events.h"

/*
 * Device-local UI — boot log console, network status, clock, and alarm.
 *
 * Four LVGL screens (log, network, clock, alarm) live alongside the server-frame
 * screen.  Button routing: enqueue_event() in llss_thread calls
 * device_ui_handle_input() first; returning true means "consumed, do not
 * forward to the LLSS server."
 *
 * Trigger: ENTER LONG_PRESS from the main (server) screen opens the log.
 * ESC returns to main.  HL_LEFT / HL_RIGHT cycle
 * log → network → clock → alarm → log.
 */

/**
 * Initialise LVGL screen objects and start on the log screen.
 * Must be called from ui_init() after LVGL is up, with lvgl_mutex held.
 * @param main_scr  The pre-existing LVGL screen that will hold server frames.
 */
void device_ui_init(lv_obj_t *main_scr);

/**
 * Thread-safe.  Appends msg to the ring buffer; if the log screen is
 * currently active, signals the render thread to refresh.
 */
void device_ui_log_push(const char *msg);

/**
 * Called from enqueue_event() before a button goes to the LLSS server.
 * Returns true if the event was consumed (must not be forwarded).
 */
bool device_ui_handle_input(enum ui_btn btn, enum ui_evt evt);

/**
 * Returns true while any device-local screen is active (not the main screen).
 */
bool device_ui_is_active(void);

/**
 * Switch the active screen back to main (server frame).
 * Called by the display thread on the first received server frame.
 * Must be called with lvgl_mutex held.
 */
void device_ui_show_main(void);

/**
 * Called from the LLSS thread after settings_subsys_init() + settings_load().
 * Enables alarm persistence (settings_save_one calls before this are no-ops).
 */
void device_ui_settings_ready(void);

#endif /* DEVICE_UI_H_ */
