/*
 * Display thread — LVGL rendering + PNG frame worker
 *
 * Owns the display semaphores, the shared PNG buffer, and all LVGL state.
 * Exposes ui_init(), ui_set_status(), and queue_display_frame() to other
 * threads.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include "display_thread.h"
#include "section_attrs.h"
#include "system_flags.h"

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

#define DISPLAY_NODE            DT_CHOSEN(zephyr_display)
#define DISPLAY_THREAD_STACK    12288
#define DISPLAY_THREAD_PRIORITY 12

/* Stack must accommodate lv_task_handler()'s recursion (LVGL traverses the
 * object tree to repaint), plus the label-set call.  2 KB overflowed and
 * corrupted neighbouring kernel state when the AP captive portal was active. */
#define STATUS_THREAD_STACK    4096
#define STATUS_THREAD_PRIORITY 13   /* lower priority than frame renderer */

/* =========================================================================
 * Shared PNG buffer (PSRAM)
 * ========================================================================= */

static uint8_t display_png_buf[CONFIG_LLSS_FRAME_BUF_SIZE]
	LLSS_EXT_RAM_NOINIT("llss_display");
static size_t display_png_len;

static K_SEM_DEFINE(display_work_sem, 0, 1);  /* signals work available */
static K_SEM_DEFINE(display_slot_sem, 1, 1);  /* limits to one pending frame */

static bool display_worker_ready;

/* =========================================================================
 * LVGL objects
 * ========================================================================= */

K_MUTEX_DEFINE(lvgl_mutex);

static lv_obj_t     *status_label;
static lv_obj_t     *sub_label;
static lv_obj_t     *frame_img;
static lv_image_dsc_t frame_dsc;

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static void ui_refresh_locked(void)
{
	lv_task_handler();

	const struct device *disp = DEVICE_DT_GET(DISPLAY_NODE);

	if (device_is_ready(disp)) {
		display_blanking_off(disp);
	}
}

static void display_png_frame_locked(const uint8_t *png_buf, size_t png_len)
{
	memset(&frame_dsc, 0, sizeof(frame_dsc));
	frame_dsc.header.cf  = LV_COLOR_FORMAT_L8;
	frame_dsc.header.w   = 800;
	frame_dsc.header.h   = 480;
	frame_dsc.data_size  = png_len;
	frame_dsc.data       = png_buf;

	if (!frame_img) {
		lv_obj_t *scr = lv_screen_active();

		frame_img = lv_image_create(scr);
		lv_obj_align(frame_img, LV_ALIGN_CENTER, 0, 0);
	}

	lv_image_set_src(frame_img, &frame_dsc);
	ui_refresh_locked();
}

/* =========================================================================
 * Thread
 * ========================================================================= */

static void display_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		k_sem_take(&display_work_sem, K_FOREVER);

		LOG_INF("Displaying frame (%zu bytes)", display_png_len);

		k_mutex_lock(&lvgl_mutex, K_FOREVER);
		display_png_frame_locked(display_png_buf, display_png_len);
		k_mutex_unlock(&lvgl_mutex);

		k_sem_give(&display_slot_sem);
	}
}

K_THREAD_DEFINE(display_thread, DISPLAY_THREAD_STACK,
		display_thread_fn, NULL, NULL, NULL,
		DISPLAY_THREAD_PRIORITY, 0, 0);

/* =========================================================================
 * Status thread — consumer of system_flags
 *
 * Wakes whenever any bit in `system_flags` changes (via K_POLL_TYPE_EVENT)
 * and re-renders a compact status string into the existing sub-label.
 * Wiring only — actual status icons / glyphs go here later.  The point of
 * this thread is to demonstrate the flag-driven render pattern without
 * polling and without coupling the producers to LVGL.
 * ========================================================================= */

static void render_status_from_flags_locked(uint32_t flags)
{
	if (!sub_label) {
		return;
	}

	char wifi_glyph[8] = "WiFi:?";

	if (flags & SYS_FLAG_WIFI_READY) {
		strncpy(wifi_glyph, "WiFi+", sizeof(wifi_glyph) - 1);
	} else if (flags & SYS_FLAG_WIFI_PROVISIONING) {
		strncpy(wifi_glyph, "WiFi*", sizeof(wifi_glyph) - 1);
	} else {
		strncpy(wifi_glyph, "WiFi-", sizeof(wifi_glyph) - 1);
	}

	const char *time_glyph = (flags & SYS_FLAG_TIME_VALID)    ? "T+" : "T-";
	const char *auth_glyph = (flags & SYS_FLAG_LLSS_AUTHORIZED) ? "A+" : "A-";

	char line[32];

	snprintf(line, sizeof(line), "%s %s %s", wifi_glyph, time_glyph, auth_glyph);
	lv_label_set_text(sub_label, line);
	ui_refresh_locked();
}

static void status_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	struct k_poll_event evt = K_POLL_EVENT_INITIALIZER(
		K_POLL_TYPE_SIGNAL,
		K_POLL_MODE_NOTIFY_ONLY,
		&system_flags_changed);

	/* Initial render so the status reflects whatever was set before this
	 * thread started (e.g. SYS_FLAG_TIME_VALID restored from RTC). */
	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	render_status_from_flags_locked(sys_flag_get());
	k_mutex_unlock(&lvgl_mutex);

	while (true) {
		k_poll(&evt, 1, K_FOREVER);
		/* k_poll signal must be reset by hand after wake-up. */
		k_poll_signal_reset(&system_flags_changed);
		evt.state = K_POLL_STATE_NOT_READY;

		uint32_t snapshot = sys_flag_get();

		k_mutex_lock(&lvgl_mutex, K_FOREVER);
		render_status_from_flags_locked(snapshot);
		k_mutex_unlock(&lvgl_mutex);
	}
}

K_THREAD_DEFINE(display_status_thread, STATUS_THREAD_STACK,
		status_thread_fn, NULL, NULL, NULL,
		STATUS_THREAD_PRIORITY, 0, 0);

/* =========================================================================
 * Public API
 * ========================================================================= */

void ui_init(void)
{
	lv_lodepng_init();

	k_mutex_lock(&lvgl_mutex, K_FOREVER);

	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

	status_label = lv_label_create(scr);
	lv_label_set_text(status_label, "Booting...");
	lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
	lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -20);

	sub_label = lv_label_create(scr);
	lv_label_set_text(sub_label, "");
	lv_obj_set_style_text_font(sub_label, &lv_font_montserrat_14, 0);
	lv_obj_align(sub_label, LV_ALIGN_CENTER, 0, 20);

	ui_refresh_locked();
	k_mutex_unlock(&lvgl_mutex);

	display_worker_ready = true;
}

void ui_set_status(const char *line1, const char *line2)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);

	if (status_label) {
		lv_label_set_text(status_label, line1 ? line1 : "");
	}
	if (sub_label) {
		lv_label_set_text(sub_label, line2 ? line2 : "");
	}
	ui_refresh_locked();
	k_mutex_unlock(&lvgl_mutex);
}

int queue_display_frame(const uint8_t *png_buf, size_t png_len)
{
	if (!display_worker_ready) {
		return -ENODEV;
	}
	if (png_len == 0 || png_len > sizeof(display_png_buf)) {
		return -EMSGSIZE;
	}

	k_sem_take(&display_slot_sem, K_FOREVER);
	memcpy(display_png_buf, png_buf, png_len);
	display_png_len = png_len;
	k_sem_give(&display_work_sem);

	return 0;
}
