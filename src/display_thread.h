#ifndef DISPLAY_THREAD_H_
#define DISPLAY_THREAD_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/**
 * LVGL mutex — must be held for any lv_* call outside display_thread.c.
 * Exposed so device_ui.c can render under the same lock.
 */
extern struct k_mutex lvgl_mutex;

/**
 * Flush the LVGL backbuffer to the display.
 * Calls lv_task_handler() then display_blanking_off().
 * Must be called with lvgl_mutex held.
 *
 * @param dither  true for device-UI screens (ordered dithering of arbitrary
 *                grays); false for pre-dithered server frames.
 */
void ui_lvgl_flush(bool dither);

/**
 * Initialise LVGL, create the main server-frame screen, start the
 * device UI on the log screen.  Call once from main() after the kernel is up.
 */
void ui_init(void);

/**
 * Append a message to the device log (console-style boot screen).
 * Thread-safe.  Replaces the old two-line ui_set_status().
 */
void ui_log_push(const char *msg);

/**
 * Zero-copy frame producer API (replaces queue_display_frame).
 *
 * Fetch the next frame directly into the buffer returned here, then call
 * display_frame_submit() with the number of bytes written.  No copy occurs:
 * the buffer is one of the display pipeline's triple-buffered SPIRAM slots.
 *
 * @param cap  If non-NULL, set to the buffer capacity in bytes.
 * @return Pointer to the current write buffer (valid until the next submit).
 */
uint8_t *display_frame_write_buf(size_t *cap);

/**
 * Publish the buffer previously obtained from display_frame_write_buf() as
 * the newest frame.  Never blocks; if the display is busy the previous
 * pending frame is dropped (latest-frame-wins).
 *
 * @param len  Number of bytes written into the write buffer.
 * @return 0 on success, -ENODEV if display not ready, -EMSGSIZE if too large.
 */
int display_frame_submit(size_t len);

#endif /* DISPLAY_THREAD_H_ */
