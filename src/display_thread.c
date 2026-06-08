/*
 * Display thread — LVGL rendering + PNG frame worker
 *
 * Owns the display semaphores, the shared PNG buffer, and the LVGL main
 * screen (server frame).  The device-local UI (log, network, clock) lives
 * in device_ui.c and shares lvgl_mutex + ui_lvgl_flush().
 *
 * Boot sequence:
 *   1. ui_init() creates the main LVGL screen and a friendly placeholder.
 *   2. device_ui builds the developer-facing menu screens, but the user
 *      remains on the main screen unless they explicitly open the menu.
 *   3. On the first server frame, display_png_frame_locked() replaces the
 *      placeholder with live server content.
 *   4. Subsequent frames update the main screen silently if device UI is
 *      open; exiting the device menu (ESC) reveals the latest frame.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include "device_ui.h"
#include "display_thread.h"
#include "material_icons.h"
#include "section_attrs.h"

/* Custom panel driver is hardware-only; on native_sim the SDL display has no
 * dither control, so guard the call out entirely. */
#if DT_HAS_COMPAT_STATUS_OKAY(custom_ssd16xx_800x480)
#include "custom_ssd16xx.h"
#endif

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

#define DISPLAY_NODE            DT_CHOSEN(zephyr_display)
#define DISPLAY_THREAD_STACK    12288
#define DISPLAY_THREAD_PRIORITY 12

/* =========================================================================
 * Triple-buffered PNG frame pipeline (PSRAM, zero-copy)
 *
 * Three SPIRAM buffers cycle through three roles so the producer (LLSS
 * thread) never blocks and never copies:
 *
 *   front  — the LLSS thread fetches the next frame straight into this
 *            buffer (HTTP body lands here directly, no memcpy)
 *   middle — "mailbox": the most recent complete frame, awaiting display
 *   back   — the buffer the display thread is currently rendering from
 *
 * submit() swaps front<->middle (publish, recycle old mailbox as next write
 * target); the worker swaps back<->middle (claim latest).  The three slots
 * are always a permutation of the three buffers, so front (being written)
 * and back (being rendered) are never the same memory.  Latest-frame-wins:
 * if the producer outruns the display, the older mailbox frame is dropped.
 * ========================================================================= */

#define FRAME_BUF_COUNT 3

static uint8_t frame_mem[FRAME_BUF_COUNT][CONFIG_LLSS_FRAME_BUF_SIZE]
	LLSS_EXT_RAM_NOINIT("llss_frames");

struct frame_slot {
	uint8_t *buf;
	size_t   len;
};

static struct frame_slot fb_front  = { .buf = frame_mem[0] };
static struct frame_slot fb_middle = { .buf = frame_mem[1] };
static struct frame_slot fb_back   = { .buf = frame_mem[2] };
static bool fb_fresh;  /* middle holds an unconsumed frame */

static K_MUTEX_DEFINE(fb_mutex);
static K_SEM_DEFINE(frame_work_sem, 0, 1);

static bool display_worker_ready;

/* =========================================================================
 * LVGL
 * ========================================================================= */

K_MUTEX_DEFINE(lvgl_mutex);

static lv_obj_t     *lvgl_main_scr;
static lv_obj_t     *frame_img;
static lv_image_dsc_t frame_dsc;
static lv_obj_t     *main_status_wrap;
static lv_obj_t     *main_status_icon;
static lv_obj_t     *main_status_title;
static lv_obj_t     *main_status_body;
static void main_status_build_locked(void)
{
	main_status_wrap = lv_obj_create(lvgl_main_scr);
	lv_obj_remove_style_all(main_status_wrap);
	lv_obj_set_size(main_status_wrap, 800, 480);
	lv_obj_clear_flag(main_status_wrap, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(main_status_wrap, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(main_status_wrap, LV_OPA_COVER, 0);

	main_status_icon = lv_label_create(main_status_wrap);
	lv_obj_set_style_text_font(main_status_icon, &material_design_120, 0);
	lv_obj_set_style_text_color(main_status_icon, lv_color_black(), 0);
	lv_label_set_text(main_status_icon, ICON_NOTIFICATION);
	lv_obj_align(main_status_icon, LV_ALIGN_TOP_MID, 0, 34);

	main_status_title = lv_label_create(main_status_wrap);
	lv_obj_set_width(main_status_title, 620);
	lv_obj_set_style_text_font(main_status_title, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_color(main_status_title, lv_color_black(), 0);
	lv_obj_set_style_text_align(main_status_title, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(main_status_title, LV_LABEL_LONG_WRAP);
	lv_label_set_text(main_status_title, "Starting device");
	lv_obj_align(main_status_title, LV_ALIGN_TOP_MID, 0, 182);

	main_status_body = lv_label_create(main_status_wrap);
	lv_obj_set_width(main_status_body, 620);
	lv_obj_set_style_text_font(main_status_body, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(main_status_body, lv_palette_main(LV_PALETTE_GREY), 0);
	lv_obj_set_style_text_align(main_status_body, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(main_status_body, LV_LABEL_LONG_WRAP);
	lv_label_set_text(main_status_body,
			  "Please wait while the device prepares its first screen.");
	lv_obj_align(main_status_body, LV_ALIGN_TOP_MID, 0, 246);
}

static void main_status_set_locked(const char *icon, const char *title,
				   const char *body)
{
	if (!main_status_wrap) {
		main_status_build_locked();
	}

	lv_label_set_text(main_status_icon,
			  (icon && icon[0]) ? icon : "");
	lv_label_set_text(main_status_title,
			  (title && title[0]) ? title : "");
	lv_label_set_text(main_status_body,
			  (body && body[0]) ? body : "");
	lv_obj_clear_flag(main_status_wrap, LV_OBJ_FLAG_HIDDEN);
	if (frame_img) {
		lv_obj_add_flag(frame_img, LV_OBJ_FLAG_HIDDEN);
	}
}

static void main_status_hide_locked(void)
{
	if (main_status_wrap) {
		lv_obj_add_flag(main_status_wrap, LV_OBJ_FLAG_HIDDEN);
	}
	if (frame_img) {
		lv_obj_clear_flag(frame_img, LV_OBJ_FLAG_HIDDEN);
	}
}

/* =========================================================================
 * Public flush — shared with device_ui.c
 * ========================================================================= */

void ui_lvgl_flush(bool dither, enum ui_refresh_ctx ctx)
{
	const struct device *disp = DEVICE_DT_GET(DISPLAY_NODE);

#if DT_HAS_COMPAT_STATUS_OKAY(custom_ssd16xx_800x480)
	/* Set before lv_task_handler() — that is what drives the driver's
	 * write() conversion for the dirty regions. Dither here means "dither
	 * arbitrary grays DOWN to 1bpp B/W" (not 4-level gray), so dithered UI
	 * stays in MONO and remains partial-refresh capable. */
	if (device_is_ready(disp)) {
		custom_ssd16xx_set_dither(disp, dither);
		custom_ssd16xx_set_color_mode(disp, CUSTOM_SSD16XX_MONO);
	}
#else
	ARG_UNUSED(dither);
#endif

	lv_task_handler();

	if (!device_is_ready(disp)) {
		return;
	}

#if DT_HAS_COMPAT_STATUS_OKAY(custom_ssd16xx_800x480)
	/* Full only when the whole image is meant to change (screen/HLSS switch,
	 * boot). For incremental updates we always request partial: the panel
	 * diffs the framebuffer itself, so a localized change is a fast partial
	 * regardless of how much LVGL repainted. The driver still upgrades to
	 * full on its own when it must (no valid previous frame, gray mode, or
	 * the periodic ghosting floor). */
	if (ctx == UI_CTX_SWITCH) {
		custom_ssd16xx_refresh_full(disp);
	} else {
		custom_ssd16xx_refresh_partial(disp);
	}
#else
	ARG_UNUSED(ctx);
	display_blanking_off(disp);
#endif
}

/* =========================================================================
 * Frame rendering
 * ========================================================================= */

static void display_png_frame_locked(const uint8_t *png_buf, size_t png_len)
{
	static bool first_frame = true;
	bool was_first = first_frame;

	if (first_frame) {
		first_frame = false;
		/* Ensure the user lands on the main screen when first content arrives. */
		device_ui_show_main();
		lv_screen_load(lvgl_main_scr);
	}

	memset(&frame_dsc, 0, sizeof(frame_dsc));
	frame_dsc.header.cf  = LV_COLOR_FORMAT_L8;
	frame_dsc.header.w   = 800;
	frame_dsc.header.h   = 480;
	frame_dsc.data_size  = png_len;
	frame_dsc.data       = png_buf;

	if (!frame_img) {
		frame_img = lv_image_create(lvgl_main_scr);
		lv_obj_align(frame_img, LV_ALIGN_CENTER, 0, 0);
	}

	lv_image_set_src(frame_img, &frame_dsc);
	main_status_hide_locked();

	/* Only flush if the main screen is actually visible. Server frames are
	 * already dithered to the panel palette — do not re-dither, and they are
	 * mono (phase 1), so they are partial-refresh capable. The very first
	 * frame also switches screens, so force a full there. */
	if (!device_ui_is_active()) {
		ui_lvgl_flush(false, was_first ? UI_CTX_SWITCH : UI_CTX_SERVER);
	}
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
		k_sem_take(&frame_work_sem, K_FOREVER);

		/* Claim the latest mailbox frame into the render slot. */
		k_mutex_lock(&fb_mutex, K_FOREVER);
		bool have = fb_fresh;

		if (have) {
			struct frame_slot tmp = fb_back;

			fb_back   = fb_middle;
			fb_middle = tmp;
			fb_fresh  = false;
		}
		k_mutex_unlock(&fb_mutex);

		if (!have) {
			continue;
		}

		LOG_INF("Displaying frame (%zu bytes)", fb_back.len);

		k_mutex_lock(&lvgl_mutex, K_FOREVER);
		display_png_frame_locked(fb_back.buf, fb_back.len);
		k_mutex_unlock(&lvgl_mutex);
	}
}

K_THREAD_DEFINE(display_thread, DISPLAY_THREAD_STACK,
		display_thread_fn, NULL, NULL, NULL,
		DISPLAY_THREAD_PRIORITY, 0, 0);

/* =========================================================================
 * Public API
 * ========================================================================= */

void ui_init(void)
{
	lv_lodepng_init();

	k_mutex_lock(&lvgl_mutex, K_FOREVER);

	lvgl_main_scr = lv_screen_active();
	lv_obj_set_style_bg_color(lvgl_main_scr, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(lvgl_main_scr, LV_OPA_COVER, 0);
	main_status_build_locked();

	/* device_ui creates and loads the log screen; keeps lvgl_mutex held */
	device_ui_init(lvgl_main_scr);
	ui_lvgl_flush(false, UI_CTX_SWITCH);

	k_mutex_unlock(&lvgl_mutex);

	display_worker_ready = true;
}

void ui_log_push(const char *msg)
{
	device_ui_log_push(msg);
}

void ui_server_status_show(const char *icon, const char *title,
			   const char *body)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	main_status_set_locked(icon, title, body);
	if (!device_ui_is_active()) {
		ui_lvgl_flush(false, UI_CTX_UI);
	}
	k_mutex_unlock(&lvgl_mutex);
}

void ui_server_status_hide(void)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	main_status_hide_locked();
	if (!device_ui_is_active()) {
		ui_lvgl_flush(false, UI_CTX_UI);
	}
	k_mutex_unlock(&lvgl_mutex);
}

uint8_t *display_frame_write_buf(size_t *cap)
{
	if (cap) {
		*cap = CONFIG_LLSS_FRAME_BUF_SIZE;
	}
	return fb_front.buf;
}

int display_frame_submit(size_t len)
{
	if (!display_worker_ready) {
		return -ENODEV;
	}
	if (len == 0 || len > CONFIG_LLSS_FRAME_BUF_SIZE) {
		return -EMSGSIZE;
	}

	/* Publish front as the new mailbox, recycle the old mailbox as the
	 * next write target.  Never blocks; latest frame wins. */
	k_mutex_lock(&fb_mutex, K_FOREVER);
	fb_front.len = len;
	struct frame_slot tmp = fb_middle;

	fb_middle = fb_front;
	fb_front  = tmp;
	fb_fresh  = true;
	k_mutex_unlock(&fb_mutex);

	k_sem_give(&frame_work_sem);
	return 0;
}
