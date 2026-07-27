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
#include "llss_client.h"
#include "material_icons.h"
#include "press_feedback.h"
#include "section_attrs.h"
#include "sys_watchdog.h"

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
	bool     full_refresh; /* server hint: full e-ink refresh for this frame */
};

static struct frame_slot fb_front  = { .buf = frame_mem[0] };
static struct frame_slot fb_middle = { .buf = frame_mem[1] };
static struct frame_slot fb_back   = { .buf = frame_mem[2] };
static bool fb_fresh;  /* middle holds an unconsumed frame */

static K_MUTEX_DEFINE(fb_mutex);
static K_SEM_DEFINE(frame_work_sem, 0, 1);

#define UI_STATUS_ICON_MAX  32
#define UI_STATUS_TITLE_MAX 96
#define UI_STATUS_BODY_MAX  192
#define UI_NOTICE_TEXT_MAX  128

struct pending_ui_updates {
	bool status_pending;
	bool status_visible;
	char status_icon[UI_STATUS_ICON_MAX];
	char status_title[UI_STATUS_TITLE_MAX];
	char status_body[UI_STATUS_BODY_MAX];
	bool notice_pending;
	bool notice_visible;
	char notice_text[UI_NOTICE_TEXT_MAX];
	/* Drop the press-feedback overlay. Needed because the overlay is otherwise
	 * only cleared by an arriving server frame, so an input answered with
	 * NO_CHANGE (the hold-hint/notice case) or an outright failure would leave
	 * the button stuck in its inverted "pressed" state forever. */
	bool press_clear_pending;
	/* Pressed-strip variant rebuild, latched by the LLSS thread after a
	 * prefetch. Applied here (not there) so the network path never blocks on
	 * lvgl_mutex behind an in-flight panel refresh — that coupling is what
	 * serialised the HTTP round-trip against the e-ink blit. Masks < 0 mean
	 * "server didn't advertise one", matching press_feedback's *_known args. */
	bool strips_pending;
	char strip_top_id[LLSS_STRIP_ID_MAX];
	char strip_bot_id[LLSS_STRIP_ID_MAX];
	int32_t strip_top_mask;
	int32_t strip_bot_mask;
};

static K_MUTEX_DEFINE(ui_req_mutex);
static struct pending_ui_updates pending_ui;

/* Pending press-feedback button, latched by ui_press_feedback_request() from
 * the input thread and consumed by the display thread. UI_BTN_NONE = none.
 * Latest-press-wins; the display thread owns the slow e-ink blit. */
static atomic_t press_fb_btn = ATOMIC_INIT(UI_BTN_NONE);

/* Latest server-frame visibility state. Read by LLSS without taking lvgl_mutex
 * so protocol progress never waits on an in-flight panel refresh. */
static atomic_t server_frame_visible = ATOMIC_INIT(0);

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
static lv_obj_t     *notice_box;   /* transient server-notice toast (hidden by default) */
static lv_obj_t     *notice_lbl;

/* How long a notice toast stays on screen before auto-dismissing. */
#define NOTICE_VISIBLE_MS 2500

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
	lv_label_set_text(main_status_icon, ICON_TERMINAL);
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
 * Notice toast — transient server-driven popup over the server frame
 * ========================================================================= */

/* Centred black pill with white Chicago text, hidden until ui_notice_show().
 * Built once on the main screen so it stacks above frame_img and clears with
 * the same lvgl_mutex discipline as everything else here. */
static void notice_build_locked(void)
{
	notice_box = status_panel(lvgl_main_scr, 620, 72, lv_color_black());
	lv_obj_align(notice_box, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_radius(notice_box, 8, 0);
	/* White frame so the black pill reads over a dark/busy board region. */
	lv_obj_set_style_border_width(notice_box, 3, 0);
	lv_obj_set_style_border_color(notice_box, lv_color_white(), 0);
	lv_obj_set_style_border_opa(notice_box, LV_OPA_COVER, 0);
	lv_obj_add_flag(notice_box, LV_OBJ_FLAG_HIDDEN);

	notice_lbl = lv_label_create(notice_box);
	lv_obj_set_width(notice_lbl, 620 - 32);
	lv_obj_set_style_text_font(notice_lbl, &chicago_18, 0);
	lv_obj_set_style_text_color(notice_lbl, lv_color_white(), 0);
	lv_obj_set_style_text_align(notice_lbl, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(notice_lbl, LV_LABEL_LONG_WRAP);
	lv_label_set_text(notice_lbl, "");
	lv_obj_center(notice_lbl);
}

static void notice_hide_locked(void)
{
	if (notice_box) {
		lv_obj_add_flag(notice_box, LV_OBJ_FLAG_HIDDEN);
	}
}

/* Fires NOTICE_VISIBLE_MS after a toast is shown: hide it and repaint. Skips
 * the flush when a device-local screen took over meanwhile (the toast lives on
 * the main screen, which isn't visible then — it will simply be hidden already
 * when the user returns). */
static void notice_hide_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&ui_req_mutex, K_FOREVER);
	pending_ui.notice_pending = true;
	pending_ui.notice_visible = false;
	k_mutex_unlock(&ui_req_mutex);
	k_sem_give(&frame_work_sem);
}

static K_WORK_DELAYABLE_DEFINE(notice_hide_work, notice_hide_work_fn);

static bool apply_pending_ui_locked(void)
{
	struct pending_ui_updates local = {0};
	bool need_flush = false;
	bool on_main = !device_ui_is_active();

	k_mutex_lock(&ui_req_mutex, K_FOREVER);
	local = pending_ui;
	memset(&pending_ui, 0, sizeof(pending_ui));
	k_mutex_unlock(&ui_req_mutex);

	if (local.status_pending) {
		if (local.status_visible) {
			main_status_set_locked(local.status_icon,
					       local.status_title,
					       local.status_body);
		} else {
			main_status_hide_locked();
		}
		need_flush = need_flush || on_main;
	}

	if (local.press_clear_pending) {
		press_feedback_hide_locked();
		need_flush = need_flush || on_main;
	}

	/* Offscreen variant rebuild only — no panel content changes, so this
	 * deliberately does not set need_flush. */
	if (local.strips_pending) {
		press_feedback_refresh_strips_locked(local.strip_top_id,
						     local.strip_bot_id);
		press_feedback_set_enabled_masks_locked(
			(uint8_t)(local.strip_top_mask & 0xFF),
			local.strip_top_mask >= 0,
			(uint8_t)(local.strip_bot_mask & 0xFF),
			local.strip_bot_mask >= 0);
	}

	if (local.notice_pending) {
		if (local.notice_visible) {
			if (notice_box && on_main) {
				lv_label_set_text(notice_lbl, local.notice_text);
				lv_obj_clear_flag(notice_box, LV_OBJ_FLAG_HIDDEN);
				lv_obj_move_foreground(notice_box);
				need_flush = true;
			}
		} else {
			notice_hide_locked();
			need_flush = need_flush || on_main;
		}
	}

	return need_flush;
}

void ui_notice_show(const char *text)
{
	if (!text || !text[0]) {
		return;
	}

	k_mutex_lock(&ui_req_mutex, K_FOREVER);
	pending_ui.notice_pending = true;
	pending_ui.notice_visible = true;
	strncpy(pending_ui.notice_text, text,
		sizeof(pending_ui.notice_text) - 1);
	pending_ui.notice_text[sizeof(pending_ui.notice_text) - 1] = '\0';
	k_mutex_unlock(&ui_req_mutex);
	k_sem_give(&frame_work_sem);

	/* (Re)arm the auto-dismiss — a fresh notice restarts the timer. */
	k_work_reschedule(&notice_hide_work, K_MSEC(NOTICE_VISIBLE_MS));
}

/* =========================================================================
 * Public flush — shared with device_ui.c
 * ========================================================================= */

#ifdef CONFIG_LLSS_EPD_TEST_SHELL
/* `epd hold on`: freeze the normal pipeline's panel access so waveform
 * experiments aren't mangled by server frames / device-UI repaints. LVGL
 * keeps accumulating dirty state and catches up on the flush after release. */
static atomic_t epd_test_hold;

void ui_test_hold(bool hold)
{
	atomic_set(&epd_test_hold, hold ? 1 : 0);
}

bool ui_test_hold_active(void)
{
	return atomic_get(&epd_test_hold) != 0;
}
#endif

void ui_lvgl_flush(bool dither, enum ui_refresh_ctx ctx)
{
	const struct device *disp = DEVICE_DT_GET(DISPLAY_NODE);

#ifdef CONFIG_LLSS_EPD_TEST_SHELL
	if (ui_test_hold_active()) {
		return;
	}
#endif

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

	/* TEMP instrumentation (BTNTRACE): splits the non-yielding CPU cost of
	 * LVGL render + the driver's write() bit-pack/dither from the panel work
	 * (SPI + waveform), to size the residual starvation of sub-priority-12
	 * threads. Remove once characterised. */
	uint32_t t_lvgl0 = k_uptime_get_32();

	lv_task_handler();

	uint32_t t_lvgl = k_uptime_get_32() - t_lvgl0;

	if (!device_is_ready(disp)) {
		return;
	}

	uint32_t t_panel0 = k_uptime_get_32();

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

	LOG_INF("BTNTRACE flush lvgl=%u panel=%u ctx=%d",
		t_lvgl, k_uptime_get_32() - t_panel0, (int)ctx);
#else
	ARG_UNUSED(ctx);
	ARG_UNUSED(t_lvgl);
	ARG_UNUSED(t_panel0);
	display_blanking_off(disp);
#endif
}

void ui_lvgl_flush_saver(enum ui_refresh_ctx ctx)
{
#if DT_HAS_COMPAT_STATUS_OKAY(custom_ssd16xx_800x480)
	const struct device *disp = DEVICE_DT_GET(DISPLAY_NODE);

#ifdef CONFIG_LLSS_EPD_TEST_SHELL
	if (ui_test_hold_active()) {
		return;
	}
#endif
	if (!device_is_ready(disp)) {
		return;
	}

	/* Pass 1 — BW baseline. Undo any standing gray pass first (coded
	 * pixels return to white, planes restored to the baseline), then
	 * render with threshold 64 so every mid-tone pixel that pass 2 will
	 * code as gray starts out white — the enhance LUT drives from white. */
	custom_ssd16xx_refresh_gray(disp, true);
	custom_ssd16xx_set_dither(disp, false);
	custom_ssd16xx_set_mono_threshold(disp, 64);
	custom_ssd16xx_set_color_mode(disp, CUSTOM_SSD16XX_MONO);
	lv_task_handler();

	if (ctx == UI_CTX_SWITCH) {
		custom_ssd16xx_refresh_full(disp);
	} else {
		custom_ssd16xx_refresh_partial(disp);
	}

	/* Pass 2 — re-render the same frame as gray codes and run the
	 * incremental enhance: anti-aliased digit edges and gray fills become
	 * two real grays instead of threshold casualties. */
	custom_ssd16xx_set_color_mode(disp, CUSTOM_SSD16XX_GRAY_MARK);
	lv_obj_invalidate(lv_screen_active());
	lv_refr_now(NULL);
	custom_ssd16xx_refresh_gray(disp, false);

	custom_ssd16xx_set_color_mode(disp, CUSTOM_SSD16XX_MONO);
	custom_ssd16xx_set_mono_threshold(disp, 128);
#else
	ui_lvgl_flush(true, ctx);
#endif
}

void ui_auto_full_refresh(bool enable)
{
#if DT_HAS_COMPAT_STATUS_OKAY(custom_ssd16xx_800x480)
	const struct device *disp = DEVICE_DT_GET(DISPLAY_NODE);

	if (device_is_ready(disp)) {
		/* enable -> restore the default ghosting floor; disable -> 0, so
		 * the app owns every full (the screensaver schedules its own). */
		custom_ssd16xx_set_full_refresh_interval(
			disp, enable ? CUSTOM_SSD16XX_DEFAULT_FULL_REFRESH_INTERVAL : 0U);
	}
#else
	ARG_UNUSED(enable);
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
static void display_frame_locked(uint8_t *slot, size_t bitmap_len,
				 bool full_refresh)
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
	atomic_set(&server_frame_visible, 1);
	main_status_hide_locked();

	/* Capture the band areas of this frame so a subsequent press can invert
	 * the right pixels (cache-miss fallback). The packed 1bpp bitmap starts
	 * just after the I1 palette prefix. */
	press_feedback_capture_locked(slot + LLSS_I1_PALETTE_BYTES);

	/* Clear any stale press-feedback overlay — the new frame supersedes it. */
	press_feedback_hide_locked();

	/* New-frame bookkeeping: resets the screensaver idle timer, counts the
	 * frame as pending, and auto-returns from the screensaver if it is showing
	 * (in which case we must load the main screen and force a full refresh). */
	bool saver_returned = device_ui_note_server_frame();

	if (saver_returned) {
		lv_screen_load(lvgl_main_scr);
	}

	/* Only flush if the main screen is actually visible. Server frames are
	 * already dithered to pure B/W by the server — do not re-dither, and they
	 * are mono, so they are partial-refresh capable. The first frame, a
	 * screensaver auto-return and a server-side full_refresh hint all force
	 * a full refresh here (UI_CTX_SWITCH); steady state stays partial. */
	if (!device_ui_is_active()) {
		const bool force_full =
			was_first || saver_returned || full_refresh;
		ui_lvgl_flush(false,
			      force_full ? UI_CTX_SWITCH : UI_CTX_SERVER);
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

	/* Renders are quick; this thread is normally parked on the frame sem. The
	 * idle/alive markers tell the liveness watchdog to watch only render time
	 * (timeout 30 s) and ignore the unbounded idle wait. */
	int disp_wdt = sys_wdt_register("display", 30000);

	while (true) {
		sys_wdt_idle(disp_wdt);
		k_sem_take(&frame_work_sem, K_FOREVER);
		sys_wdt_alive(disp_wdt);

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

		enum ui_btn fb_btn =
			(enum ui_btn)atomic_set(&press_fb_btn, UI_BTN_NONE);
		bool need_ui_flush;

		k_mutex_lock(&lvgl_mutex, K_FOREVER);
		need_ui_flush = apply_pending_ui_locked();

		if (have) {
			LOG_INF("Displaying frame (%zu bytes, %s)",
				fb_back.len, fb_back.full_refresh ? "full" : "partial");
			display_frame_locked(fb_back.buf, fb_back.len,
					     fb_back.full_refresh);
		} else if (need_ui_flush) {
			ui_lvgl_flush(false, UI_CTX_UI);
		}
		k_mutex_unlock(&lvgl_mutex);

		if (!have && fb_btn != UI_BTN_NONE) {
			press_feedback_show(fb_btn);
		}
	}
}

K_THREAD_DEFINE(display_thread, DISPLAY_THREAD_STACK,
		display_thread_fn, NULL, NULL, NULL,
		DISPLAY_THREAD_PRIORITY, 0, 0);

/* =========================================================================
 * Public API
 * ========================================================================= */

#if DT_HAS_COMPAT_STATUS_OKAY(custom_ssd16xx_800x480)
/*
 * Flush callback replacing Zephyr's lvgl_flush_cb_mono().
 *
 * The stock one always calls lvgl_transform_buffer(), a per-pixel loop over the
 * whole area (plus a full-buffer memset and memcpy) that re-lays-out the bits.
 * For this panel that transform is an identity: we advertise
 * SCREEN_INFO_MONO_MSB_FIRST with horizontal tiling, LVGL renders I1 MSB-first,
 * and LV_DRAW_BUF_STRIDE_ALIGN is 1 so the row stride is already 100 bytes. So
 * we hand LVGL's buffer straight to the driver, whose mono write() is a memcpy
 * into the planes — zero conversions from LVGL's canvas to the glass.
 *
 * Deliberately omits the stock callback's display_blanking_on/off dance: the
 * panel refresh is driven by ui_lvgl_flush() after lv_task_handler() returns,
 * not by LVGL, so blanking here would trigger refreshes we do not want.
 */
static void llss_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
			       uint8_t *px_map)
{
	const struct device *display_dev = DEVICE_DT_GET(DISPLAY_NODE);
	uint16_t w = area->x2 - area->x1 + 1;
	uint16_t h = area->y2 - area->y1 + 1;

	/* LVGL reserves a 2-entry palette ahead of an I1 buffer. */
	px_map += 2 * sizeof(lv_color32_t);

	struct display_buffer_descriptor desc = {
		.buf_size = ((uint32_t)w * h) / 8U,
		.width    = w,
		.pitch    = w,
		.height   = h,
		.frame_incomplete = !lv_display_flush_is_last(disp),
	};

	(void)display_write(display_dev, area->x1, area->y1, &desc, px_map);
	lv_display_flush_ready(disp);
}
#endif

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

#if DT_HAS_COMPAT_STATUS_OKAY(custom_ssd16xx_800x480)
	/* Swap in the transform-free flush path (see llss_lvgl_flush_cb). Zephyr's
	 * auto-init already built the mono display + rounder for us; only the
	 * flush callback needs replacing. */
	lv_display_set_flush_cb(lv_display_get_default(), llss_lvgl_flush_cb);
#endif

	lvgl_main_scr = lv_screen_active();
	lv_obj_set_style_bg_color(lvgl_main_scr, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(lvgl_main_scr, LV_OPA_COVER, 0);
	main_status_build_locked();

	/* device_ui creates and loads the log screen; keeps lvgl_mutex held */
	device_ui_init(lvgl_main_scr);

	/* Press-feedback overlays sit on the main server-frame screen so they
	 * stack above frame_img and clear naturally when a new frame submits. */
	press_feedback_init(lvgl_main_scr);

	/* Notice toast sits on the same screen, above everything, hidden until
	 * a server "notice" arrives. */
	notice_build_locked();

	ui_lvgl_flush(false, UI_CTX_SWITCH);

	k_mutex_unlock(&lvgl_mutex);

	display_worker_ready = true;
}

void ui_log_push(const char *msg)
{
	device_ui_log_push(msg);
}

void ui_press_feedback_update_strips(const char *top_id, const char *bot_id,
				     int32_t top_mask, int32_t bot_mask)
{
	k_mutex_lock(&ui_req_mutex, K_FOREVER);
	pending_ui.strips_pending = true;
	strncpy(pending_ui.strip_top_id, top_id ? top_id : "",
		sizeof(pending_ui.strip_top_id) - 1);
	pending_ui.strip_top_id[sizeof(pending_ui.strip_top_id) - 1] = '\0';
	strncpy(pending_ui.strip_bot_id, bot_id ? bot_id : "",
		sizeof(pending_ui.strip_bot_id) - 1);
	pending_ui.strip_bot_id[sizeof(pending_ui.strip_bot_id) - 1] = '\0';
	pending_ui.strip_top_mask = top_mask;
	pending_ui.strip_bot_mask = bot_mask;
	k_mutex_unlock(&ui_req_mutex);

	k_sem_give(&frame_work_sem);
}

void ui_press_feedback_clear(void)
{
	k_mutex_lock(&ui_req_mutex, K_FOREVER);
	pending_ui.press_clear_pending = true;
	k_mutex_unlock(&ui_req_mutex);

	k_sem_give(&frame_work_sem);
}

void ui_press_feedback_request(enum ui_btn btn)
{
	if (btn == UI_BTN_NONE) {
		return;
	}

	/* Latch the button and wake the display thread. Fast + non-blocking so
	 * the input callback returns at once; the display thread does the actual
	 * e-ink blit. frame_work_sem coalesces (max 1) — the loop checks both the
	 * press-fb latch and the frame mailbox each wake, so a coincident frame
	 * submit is not lost. */
	LOG_INF("BTNTRACE display request btn=%d t=%u",
		(int)btn, k_uptime_get_32());
	atomic_set(&press_fb_btn, (atomic_val_t)btn);
	k_sem_give(&frame_work_sem);
}

void ui_server_status_show(const char *icon, const char *title,
			   const char *body)
{
	k_mutex_lock(&ui_req_mutex, K_FOREVER);
	pending_ui.status_pending = true;
	pending_ui.status_visible = true;
	strncpy(pending_ui.status_icon, (icon && icon[0]) ? icon : "",
		sizeof(pending_ui.status_icon) - 1);
	pending_ui.status_icon[sizeof(pending_ui.status_icon) - 1] = '\0';
	strncpy(pending_ui.status_title, (title && title[0]) ? title : "",
		sizeof(pending_ui.status_title) - 1);
	pending_ui.status_title[sizeof(pending_ui.status_title) - 1] = '\0';
	strncpy(pending_ui.status_body, (body && body[0]) ? body : "",
		sizeof(pending_ui.status_body) - 1);
	pending_ui.status_body[sizeof(pending_ui.status_body) - 1] = '\0';
	k_mutex_unlock(&ui_req_mutex);
	k_sem_give(&frame_work_sem);
}

void ui_server_status_hide(void)
{
	k_mutex_lock(&ui_req_mutex, K_FOREVER);
	pending_ui.status_pending = true;
	pending_ui.status_visible = false;
	k_mutex_unlock(&ui_req_mutex);
	k_sem_give(&frame_work_sem);
}

bool ui_has_server_frame(void)
{
	return atomic_get(&server_frame_visible) != 0;
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

int display_frame_submit(size_t len, bool full_refresh)
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
	fb_front.len          = len;
	fb_front.full_refresh = full_refresh;
	struct frame_slot tmp = fb_middle;

	fb_middle = fb_front;
	fb_front  = tmp;
	fb_fresh  = true;
	k_mutex_unlock(&fb_mutex);

	k_sem_give(&frame_work_sem);
	return 0;
}
