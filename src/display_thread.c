/*
 * Display thread — LVGL rendering + server frame worker
 *
 * Owns the display semaphores, the shared frame buffers, and the LVGL main
 * screen (server frame).  The device-local UI (log, network, clock) lives
 * in device_ui.c and shares lvgl_mutex + ui_lvgl_flush().
 *
 * Server frames are the panel-native 1bpp packed bitmap (fetched with
 * ?raw=true) and are shown via LVGL as a native I1 image — no PNG decode.
 *
 * Boot sequence:
 *   1. ui_init() creates the main LVGL screen and a friendly placeholder.
 *   2. device_ui builds the developer-facing menu screens, but the user
 *      remains on the main screen unless they explicitly open the menu.
 *   3. On the first server frame, display_frame_locked() replaces the
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

#if defined(CONFIG_LLSS_PATTERN_TEST)
#include "pattern_check.h"
#endif

/* Custom panel driver is hardware-only; on native_sim the SDL display has no
 * dither control, so guard the call out entirely. */
#if DT_HAS_COMPAT_STATUS_OKAY(custom_ssd16xx_800x480)
#include "custom_ssd16xx.h"
#endif

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

LV_FONT_DECLARE(chicago_18);   /* Mac title  (shared with device_ui) */
LV_FONT_DECLARE(geneva_14);    /* Mac body   (shared with device_ui) */

#define DISPLAY_NODE            DT_CHOSEN(zephyr_display)
#define DISPLAY_THREAD_STACK    12288
#define DISPLAY_THREAD_PRIORITY 12

/* =========================================================================
 * Triple-buffered server frame pipeline (PSRAM, zero-copy)
 *
 * Three SPIRAM buffers cycle through three roles so the producer (LLSS
 * thread) never blocks and never copies:
 *
 *   front  — the LLSS thread fetches the next packed frame straight into this
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

/* Server frames arrive as the panel-native 1bpp packed bitmap (?raw=true) and
 * are handed to LVGL as a native I1 image — no PNG decode. An I1 image expects
 * a 2-entry palette (2 * lv_color32_t = 8 bytes) immediately BEFORE the bitmap,
 * so we reserve that many bytes at the start of every slot and land the HTTP
 * body just after it. */
#define LLSS_I1_PALETTE_BYTES (2 * (int)sizeof(lv_color32_t)) /* 8 */

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
static bool          server_frame_visible;

/* Bare opaque child panel — no border/padding/scroll. */
static lv_obj_t *status_panel(lv_obj_t *parent, int32_t w, int32_t h,
			      lv_color_t bg)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_remove_style_all(o);
	lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_size(o, w, h);
	lv_obj_set_style_bg_color(o, bg, 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	return o;
}

/* Mac-style "server status" splash: a centred framed dialog (white box, 2px
 * black border, hard 3px drop shadow) holding the big status icon, a Chicago
 * title and a Geneva body — consistent with the device menu and HLSS dialogs. */
static void main_status_build_locked(void)
{
	const int32_t bx = 130, by = 80, bw = 540, bh = 320;

	main_status_wrap = status_panel(lvgl_main_scr, 800, 480, lv_color_white());

	/* hard drop shadow then the framed white dialog box */
	lv_obj_t *shadow = status_panel(main_status_wrap, bw, bh, lv_color_black());

	lv_obj_set_pos(shadow, bx + 3, by + 3);

	lv_obj_t *box = status_panel(main_status_wrap, bw, bh, lv_color_white());

	lv_obj_set_pos(box, bx, by);
	lv_obj_set_style_border_width(box, 2, 0);
	lv_obj_set_style_border_color(box, lv_color_black(), 0);
	lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);

	main_status_icon = lv_label_create(box);
	lv_obj_set_style_text_font(main_status_icon, &material_design_120, 0);
	lv_obj_set_style_text_color(main_status_icon, lv_color_black(), 0);
	lv_label_set_text(main_status_icon, ICON_NOTIFICATION);
	lv_obj_align(main_status_icon, LV_ALIGN_TOP_MID, 0, 18);

	main_status_title = lv_label_create(box);
	lv_obj_set_width(main_status_title, bw - 48);
	lv_obj_set_style_text_font(main_status_title, &chicago_18, 0);
	lv_obj_set_style_text_color(main_status_title, lv_color_black(), 0);
	lv_obj_set_style_text_align(main_status_title, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(main_status_title, LV_LABEL_LONG_WRAP);
	lv_label_set_text(main_status_title, "Iniciando dispositivo");
	lv_obj_align(main_status_title, LV_ALIGN_TOP_MID, 0, 162);

	main_status_body = lv_label_create(box);
	lv_obj_set_width(main_status_body, bw - 64);
	lv_obj_set_style_text_font(main_status_body, &geneva_14, 0);
	lv_obj_set_style_text_color(main_status_body, lv_color_black(), 0);
	lv_obj_set_style_text_align(main_status_body, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(main_status_body, LV_LABEL_LONG_WRAP);
	lv_label_set_text(main_status_body,
			  "Aguarde enquanto o dispositivo prepara a primeira tela.");
	lv_obj_align(main_status_body, LV_ALIGN_TOP_MID, 0, 210);
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

/* Index 0 = black, index 1 = white. The backend packs the 1bpp bitmap MSB-first
 * with bit 1 = white, which matches LVGL's I1 bit order, so this palette maps
 * the bits straight to the right shade. lv_color32_t is {blue, green, red,
 * alpha}; for a grayscale panel R==G==B is all that matters. */
static const lv_color32_t i1_palette[2] = {
	{ .blue = 0x00, .green = 0x00, .red = 0x00, .alpha = 0xFF }, /* 0: black */
	{ .blue = 0xFF, .green = 0xFF, .red = 0xFF, .alpha = 0xFF }, /* 1: white */
};

/*
 * Render a server frame. `slot` is a frame buffer whose first
 * LLSS_I1_PALETTE_BYTES are reserved for the I1 palette and whose packed 1bpp
 * bitmap (bitmap_len bytes) follows. We present it to LVGL as a native I1
 * image: LVGL decodes it line-by-line on demand (image cache + RAM-load are
 * disabled), so there is no full-screen RGB expansion and no PNG decode.
 */
static void display_frame_locked(uint8_t *slot, size_t bitmap_len)
{
	static bool first_frame = true;
	bool was_first = first_frame;

	if (first_frame) {
		first_frame = false;
		/* Ensure the user lands on the main screen when first content arrives. */
		device_ui_show_main();
		lv_screen_load(lvgl_main_scr);
	}

#if defined(CONFIG_LLSS_PATTERN_TEST)
	/* CP2 — verify the bytes after the triple-buffer swap, i.e. exactly what
	 * we are about to hand to LVGL. If CP1 passed but CP2 fails, the swap
	 * logic handed us the wrong/stale/overwritten slot. The body sits after
	 * the reserved I1 palette; the canary is in the slot slack after it. */
	(void)pattern_verify(slot + LLSS_I1_PALETTE_BYTES, bitmap_len, "CP2-render");
	if (bitmap_len == PATTERN_BYTES) {
		/* +1 skips the HTTP-layer NUL terminator that lands on the first
		 * slack byte (see CP1-canary note). */
		size_t used = LLSS_I1_PALETTE_BYTES + PATTERN_BYTES + 1;

		if (CONFIG_LLSS_FRAME_BUF_SIZE > used) {
			(void)pattern_canary_verify(slot + used,
						    CONFIG_LLSS_FRAME_BUF_SIZE - used,
						    "CP2-canary");
		}
	}
#endif

	/* Fill the palette LVGL expects immediately before the bitmap. */
	memcpy(slot, i1_palette, sizeof(i1_palette));

	memset(&frame_dsc, 0, sizeof(frame_dsc));
	frame_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
	frame_dsc.header.cf     = LV_COLOR_FORMAT_I1;
	frame_dsc.header.w      = 800;
	frame_dsc.header.h      = 480;
	frame_dsc.header.stride = 800 / 8;          /* 100 bytes per row */
	frame_dsc.data          = slot;             /* palette + bitmap, contiguous */
	frame_dsc.data_size     = sizeof(i1_palette) + bitmap_len;

	if (!frame_img) {
		frame_img = lv_image_create(lvgl_main_scr);
		lv_obj_align(frame_img, LV_ALIGN_CENTER, 0, 0);
	}

	lv_image_set_src(frame_img, &frame_dsc);
	server_frame_visible = true;
	main_status_hide_locked();

	/* Only flush if the main screen is actually visible. Server frames are
	 * already dithered to pure B/W by the server — do not re-dither, and they
	 * are mono, so they are partial-refresh capable. The very first frame also
	 * switches screens, so force a full there. */
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
		display_frame_locked(fb_back.buf, fb_back.len);
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
#if defined(CONFIG_LLSS_PATTERN_TEST)
	/* Lay down a canary in the slack of every slot (after palette + 48000 B
	 * body) so any write that spills past the frame body is detectable. */
	size_t used = LLSS_I1_PALETTE_BYTES + PATTERN_BYTES;

	if (CONFIG_LLSS_FRAME_BUF_SIZE > used) {
		for (int i = 0; i < FRAME_BUF_COUNT; i++) {
			pattern_canary_fill(frame_mem[i] + used,
					    CONFIG_LLSS_FRAME_BUF_SIZE - used);
		}
	}
#endif

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

bool ui_has_server_frame(void)
{
	bool have_frame;

	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	have_frame = server_frame_visible;
	k_mutex_unlock(&lvgl_mutex);

	return have_frame;
}

uint8_t *display_frame_write_buf(size_t *cap)
{
	if (cap) {
		*cap = CONFIG_LLSS_FRAME_BUF_SIZE - LLSS_I1_PALETTE_BYTES;
	}
	/* Body lands after the reserved I1 palette space; the palette is filled
	 * in at render time (display_frame_locked). */
	return fb_front.buf + LLSS_I1_PALETTE_BYTES;
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
