#ifndef DISPLAY_THREAD_H_
#define DISPLAY_THREAD_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "input_events.h"

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
 * Screensaver flush with incremental grayscale: renders the BW baseline
 * (threshold 64, no dither), refreshes (full for UI_CTX_SWITCH, else
 * partial), then re-renders the frame as gray codes and runs the enhance
 * waveform — anti-aliased digits get two real grays. Reverts any standing
 * gray pass first, so it is safe to call every tick. Must hold lvgl_mutex.
 */
void ui_lvgl_flush_saver(enum ui_refresh_ctx ctx);

/**
 * Enable/disable the panel's automatic full-refresh floor (the ghosting
 * backstop that force-promotes a partial to full every N partials).
 *
 * Disable (false) when a screen owns its own flash schedule — e.g. the
 * screensaver clock, which flashes only on the wall-clock half-hour and must
 * not be interrupted by an off-schedule auto-full. Re-enable (true) on exit to
 * restore the default ghosting backstop. Scheduled flashes meanwhile go
 * through ui_lvgl_flush(_, UI_CTX_SWITCH). Thread-safe; takes effect next refresh.
 */
void ui_auto_full_refresh(bool enable);

#ifdef CONFIG_LLSS_EPD_TEST_SHELL
/**
 * Waveform test hold (`epd hold on|off`): while active, ui_lvgl_flush()
 * returns immediately, so server frames and device-UI repaints neither touch
 * the panel nor overwrite the driver's plane buffers mid-experiment. Content
 * keeps flowing upstream and catches up on the first flush after release.
 */
void ui_test_hold(bool hold);
bool ui_test_hold_active(void);
#endif

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
 * Request a local press-feedback overlay for @p btn, rendered by the display
 * thread. Called from the input-callback thread on key-down; returns
 * immediately (just latches the button and wakes the display thread). The slow
 * e-ink flush then runs on the display thread — never on the caller — so it
 * cannot block the input path and skew long-press timing. Latest press wins if
 * several arrive before the display thread services them.
 */
void ui_press_feedback_request(enum ui_btn btn);

/**
 * Drop the press-feedback overlay, returning the button to its normal look.
 *
 * The overlay is otherwise only cleared when a new server frame lands, so an
 * input the server answers with NO_CHANGE (the hold-hint/notice case) — or one
 * that fails outright — would leave the slot inverted permanently. Call once
 * the input round-trip has finished and no frame is coming.
 *
 * Non-blocking; the display thread does the repaint.
 */
void ui_press_feedback_clear(void);

/**
 * Latch newly-prefetched pressed-strip ids + enabled-slot masks for the display
 * thread to apply under @ref lvgl_mutex.
 *
 * Called from the LLSS thread after a strip prefetch. It must not take
 * lvgl_mutex itself: doing so parks the network path behind an in-flight e-ink
 * refresh, which is exactly what serialised the input HTTP round-trip against
 * the press-feedback blit. Non-blocking; latest values win.
 *
 * @param top_id/bot_id  strip ids, "" or NULL for none.
 * @param top_mask/bot_mask  enabled-slot bitmasks, or <0 when the server did
 *                           not advertise one (press_feedback then keeps its
 *                           capture-time heuristic).
 */
void ui_press_feedback_update_strips(const char *top_id, const char *bot_id,
				     int32_t top_mask, int32_t bot_mask);

/**
 * Update the user-facing placeholder shown on the main screen while no
 * server frame is available yet.
 *
 * The request is latched and applied by the display thread, so callers never
 * wait on an e-paper refresh. Passing NULL or an empty string for
 * icon/title/body leaves that field blank.
 */
void ui_server_status_show(const char *icon, const char *title,
			   const char *body);

/**
 * Hide the main-screen placeholder so the latest server frame is shown.
 *
 * The hide is queued to the display thread; callers do not block on the
 * panel refresh.
 */
void ui_server_status_hide(void);

/**
 * Flash a transient, self-dismissing popup ("notice") over the server frame —
 * a centred black toast with white text, e.g. "hold the button to offer a
 * draw". Used for server-driven hints that are not tied to a frame (they ride
 * a NO_CHANGE input response). Auto-hides after a couple of seconds.
 *
 * Thread-safe. The request is queued to the display thread, which coalesces it
 * with any pending frame refresh. No-op while a device-local screen
 * (menu/screensaver) is active. Passing NULL or an empty string is a no-op.
 */
void ui_notice_show(const char *text);

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
 * @param full_refresh  When true, the next flush uses UI_CTX_SWITCH (full
 *                      e-ink refresh) instead of UI_CTX_SERVER (partial).
 *                      Use for frames where ghosting from a partial
 *                      refresh would be objectionable (move applied, view
 *                      mode change, etc.).
 * @return 0 on success, -ENODEV if display not ready, -EMSGSIZE if too large.
 */
int display_frame_submit(size_t len, bool full_refresh);

#endif /* DISPLAY_THREAD_H_ */
