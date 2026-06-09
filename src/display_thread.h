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
 * Refresh context — tells the flush layer whether to force a full refresh.
 * Full is a semantic choice (the whole image is meant to change), never an
 * "amount changed" guess: the panel diffs the framebuffer itself, so a small
 * in-place change is a fast partial even if LVGL repainted the whole screen.
 *
 *  UI_CTX_SWITCH  screen/HLSS switch, boot, color-mode change: always full.
 *  UI_CTX_UI      device-UI in-place repaint (clock tick, new log line):
 *                 partial; driver's periodic floor is the ghosting backstop.
 *  UI_CTX_SERVER  whole-image server frame: partial (same floor backstop).
 *                 This is the seam where a future server-side diff hint
 *                 ("this frame changed a lot -> full") gets injected.
 */
enum ui_refresh_ctx {
	UI_CTX_SWITCH = 0,
	UI_CTX_UI,
	UI_CTX_SERVER,
};

/**
 * Flush the LVGL backbuffer to the display, choosing partial vs full refresh.
 * Calls lv_task_handler() then the appropriate panel refresh.
 * Must be called with lvgl_mutex held.
 *
 * @param dither  true for device-UI screens with arbitrary grays — ordered-
 *                dithered DOWN to 1bpp B/W, still partial-capable; false for
 *                already-dithered content (server frames) that must not be
 *                re-dithered. Both stay mono/partial-capable.
 * @param ctx     refresh context, see enum ui_refresh_ctx.
 */
void ui_lvgl_flush(bool dither, enum ui_refresh_ctx ctx);

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
 * Update the user-facing placeholder shown on the main screen while no
 * server frame is available yet.
 *
 * Passing NULL or an empty string for icon/title/body leaves that field blank.
 */
void ui_server_status_show(const char *icon, const char *title,
			   const char *body);

/**
 * Hide the main-screen placeholder so the latest server frame is shown.
 */
void ui_server_status_hide(void);

/**
 * Return true once at least one server frame has been rendered.
 */
bool ui_has_server_frame(void);

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
