/*
 * Display thread — LVGL rendering + PNG frame worker
 *
 * Owns the display semaphores, the shared PNG buffer, and the LVGL main
 * screen (server frame).  The device-local UI (log, network, clock) lives
 * in device_ui.c and shares lvgl_mutex + ui_lvgl_flush().
 *
 * Boot sequence:
 *   1. ui_init() creates the main LVGL screen and hands it to device_ui,
 *      which immediately loads the log screen (console-style boot view).
 *   2. On the first server frame, display_png_frame_locked() switches back
 *      to the main screen.
 *   3. Subsequent frames update the main screen silently if device UI is open;
 *      exiting the device menu (ESC) reveals the latest frame.
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

/* =========================================================================
 * Public flush — shared with device_ui.c
 * ========================================================================= */

void ui_lvgl_flush(bool dither)
{
	const struct device *disp = DEVICE_DT_GET(DISPLAY_NODE);

#if DT_HAS_COMPAT_STATUS_OKAY(custom_ssd16xx_800x480)
	/* Set before lv_task_handler() — that is what drives the driver's
	 * write() (L8->2bpp conversion) for the dirty regions. */
	if (device_is_ready(disp)) {
		custom_ssd16xx_set_dither(disp, dither);
	}
#else
	ARG_UNUSED(dither);
#endif

	lv_task_handler();

	if (device_is_ready(disp)) {
		display_blanking_off(disp);
	}
}

/* =========================================================================
 * Frame rendering
 * ========================================================================= */

static void display_png_frame_locked(const uint8_t *png_buf, size_t png_len)
{
	static bool first_frame = true;

	if (first_frame) {
		first_frame = false;
		/* Exit boot log → show main screen */
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

	/* Only flush if the main screen is actually visible.  Server frames are
	 * already dithered — do not re-dither. */
	if (!device_ui_is_active()) {
		ui_lvgl_flush(false);
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

	/* device_ui creates and loads the log screen; keeps lvgl_mutex held */
	device_ui_init(lvgl_main_scr);

	k_mutex_unlock(&lvgl_mutex);

	display_worker_ready = true;
}

void ui_log_push(const char *msg)
{
	device_ui_log_push(msg);
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
