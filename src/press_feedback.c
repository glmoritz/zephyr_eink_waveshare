#include "press_feedback.h"

#include "device_ui.h"
#include "display_thread.h"
#include "llss_strip_cache.h"
#include "section_attrs.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(press_feedback, LOG_LEVEL_INF);

#define DISPLAY_W        800
#define DISPLAY_H        480
#define ROW_BYTES        (DISPLAY_W / 8) /* 100 */
#define TOP_H            CONFIG_LLSS_TOP_STRIP_HEIGHT
#define BOT_H            CONFIG_LLSS_BOTTOM_STRIP_HEIGHT
#define SLOTS            8
#define SLOT_BITS        (DISPLAY_W / SLOTS) /* 100 */

#define I1_PALETTE_BYTES (2 * (int)sizeof(lv_color32_t)) /* 8 */
#define TOP_BITMAP_BYTES (DISPLAY_W * TOP_H / 8)
#define BOT_BITMAP_BYTES (DISPLAY_W * BOT_H / 8)

enum band { BAND_TOP = 0, BAND_BOT, BAND_NONE };

/* Button → (band, slot). Hardware contract (2026-06-29):
 *   bottom strip = btn1..btn8 across slots 0..7 (BTN_1..BTN_8)
 *   top strip    = btn9..btn16 across slots 0..7
 *     slot 0 (btn9)  : no hardware
 *     slot 1 (btn10) : LOCAL menu trigger — handled by device_ui
 *     slot 2 (btn11) : local-only, app rendering space
 *     slot 3 (btn12) : local-only, app rendering space
 *     slot 4 (btn13) : HL_LEFT
 *     slot 5 (btn14) : HL_RIGHT
 *     slot 6 (btn15) : ENTER
 *     slot 7 (btn16) : ESC
 */
struct slot_pos { enum band band; int slot; };
static const struct slot_pos slot_table[UI_BTN_COUNT] = {
	[UI_BTN_1]        = { BAND_BOT, 0 },
	[UI_BTN_2]        = { BAND_BOT, 1 },
	[UI_BTN_3]        = { BAND_BOT, 2 },
	[UI_BTN_4]        = { BAND_BOT, 3 },
	[UI_BTN_5]        = { BAND_BOT, 4 },
	[UI_BTN_6]        = { BAND_BOT, 5 },
	[UI_BTN_7]        = { BAND_BOT, 6 },
	[UI_BTN_8]        = { BAND_BOT, 7 },
	[UI_BTN_HL_LEFT]  = { BAND_TOP, 4 },
	[UI_BTN_HL_RIGHT] = { BAND_TOP, 5 },
	[UI_BTN_ENTER]    = { BAND_TOP, 6 },
	[UI_BTN_ESC]      = { BAND_TOP, 7 },
	/* MENU is local-only: device_ui owns the visual. Without this
	 * explicit entry the designated-initializer-zero default would land
	 * on { BAND_TOP, 0 } and we'd flash the (empty) slot 0 on a menu
	 * keydown. */
	[UI_BTN_MENU]     = { BAND_NONE, 0 },
};

/* Slots that can receive a press from the HLSS-mapped top buttons (the
 * device-local menu trigger at slot 1 doesn't show LLSS press feedback —
 * device_ui draws its own visual). */
static const int top_usable_slots[] = { 4, 5, 6, 7 };

static const lv_color32_t i1_palette[2] = {
	{ .blue = 0x00, .green = 0x00, .red = 0x00, .alpha = 0xFF },
	{ .blue = 0xFF, .green = 0xFF, .red = 0xFF, .alpha = 0xFF },
};

/* Per-slot pre-computed inverted band, ready to blit. */
struct top_band_overlay {
	uint8_t        buf[I1_PALETTE_BYTES + TOP_BITMAP_BYTES];
	lv_image_dsc_t dsc;
};
struct bot_band_overlay {
	uint8_t        buf[I1_PALETTE_BYTES + BOT_BITMAP_BYTES];
	lv_image_dsc_t dsc;
};

/* Captured band bytes from the most recent frame (bitmap only). The variants
 * are derived from these; kept persistent so refresh_strips can reconstruct
 * any variant without needing the original frame buffer back. */
static uint8_t top_orig[TOP_BITMAP_BYTES] LLSS_EXT_RAM_NOINIT("press_top_orig");
static uint8_t bot_orig[BOT_BITMAP_BYTES] LLSS_EXT_RAM_NOINIT("press_bot_orig");

static struct top_band_overlay top_inv[SLOTS]
	LLSS_EXT_RAM_NOINIT("press_top_inv");
static struct bot_band_overlay bot_inv[SLOTS]
	LLSS_EXT_RAM_NOINIT("press_bot_inv");

static bool      captured;
static lv_obj_t *top_img;
static lv_obj_t *bot_img;

/* Enabled-slot bitmasks: bit S = "pressing slot S should show feedback".
 * Default at capture time comes from a cheap all-white scan of each slot
 * (no button drawn => no feedback). HLSS strip-id metadata can override
 * via press_feedback_set_enabled_masks_locked once strips land. */
static uint8_t top_enabled_mask;
static uint8_t bot_enabled_mask;

static void init_top_dsc(lv_image_dsc_t *d, uint8_t *buf)
{
	memset(d, 0, sizeof(*d));
	d->header.magic  = LV_IMAGE_HEADER_MAGIC;
	d->header.cf     = LV_COLOR_FORMAT_I1;
	d->header.w      = DISPLAY_W;
	d->header.h      = TOP_H;
	d->header.stride = ROW_BYTES;
	d->data          = buf;
	d->data_size     = I1_PALETTE_BYTES + TOP_BITMAP_BYTES;
}

static void init_bot_dsc(lv_image_dsc_t *d, uint8_t *buf)
{
	memset(d, 0, sizeof(*d));
	d->header.magic  = LV_IMAGE_HEADER_MAGIC;
	d->header.cf     = LV_COLOR_FORMAT_I1;
	d->header.w      = DISPLAY_W;
	d->header.h      = BOT_H;
	d->header.stride = ROW_BYTES;
	d->data          = buf;
	d->data_size     = I1_PALETTE_BYTES + BOT_BITMAP_BYTES;
}

void press_feedback_init(lv_obj_t *parent)
{
	if (!parent) {
		return;
	}

	for (int i = 0; i < SLOTS; i++) {
		memcpy(top_inv[i].buf, i1_palette, sizeof(i1_palette));
		init_top_dsc(&top_inv[i].dsc, top_inv[i].buf);
		memcpy(bot_inv[i].buf, i1_palette, sizeof(i1_palette));
		init_bot_dsc(&bot_inv[i].dsc, bot_inv[i].buf);
	}

	top_img = lv_image_create(parent);
	lv_obj_set_pos(top_img, 0, 0);
	lv_obj_add_flag(top_img, LV_OBJ_FLAG_HIDDEN);

	bot_img = lv_image_create(parent);
	lv_obj_set_pos(bot_img, 0, DISPLAY_H - BOT_H);
	lv_obj_add_flag(bot_img, LV_OBJ_FLAG_HIDDEN);

	captured = false;
}

/* True when every pixel of the slot's column range in the band is white
 * (bit set in MSB-first I1) — i.e. HLSS drew no button there, so a press
 * on this slot should produce no local feedback. */
static bool slot_is_blank(const uint8_t *bitmap, int height, int slot)
{
	const int start_col = slot * SLOT_BITS;
	const int end_col   = start_col + SLOT_BITS;

	for (int y = 0; y < height; y++) {
		const uint8_t *row = bitmap + y * ROW_BYTES;

		for (int c = start_col; c < end_col; c++) {
			if (!(row[c >> 3] & (uint8_t)(0x80U >> (c & 7)))) {
				return false; /* hit a black pixel */
			}
		}
	}
	return true;
}

static uint8_t scan_enabled_mask(const uint8_t *bitmap, int height,
				 const int *slots, size_t n_slots)
{
	uint8_t mask = 0;

	for (size_t i = 0; i < n_slots; i++) {
		if (!slot_is_blank(bitmap, height, slots[i])) {
			mask |= (uint8_t)(1U << slots[i]);
		}
	}
	return mask;
}

/* XOR every bit of the slot's 100-pixel column range, row by row.
 * MSB-first within bytes (LVGL I1 default). */
static void invert_slot(uint8_t *bitmap, int height, int slot)
{
	const int start_col = slot * SLOT_BITS;
	const int end_col   = start_col + SLOT_BITS;

	for (int y = 0; y < height; y++) {
		uint8_t *row = bitmap + y * ROW_BYTES;

		for (int c = start_col; c < end_col; c++) {
			row[c >> 3] ^= (uint8_t)(0x80U >> (c & 7));
		}
	}
}

/* Replace the slot's column range in @p dst with the equivalent bits from
 * @p src. Both buffers are bitmap-only (no palette). */
static void replace_slot(uint8_t *dst, const uint8_t *src, int height, int slot)
{
	const int start_col = slot * SLOT_BITS;
	const int end_col   = start_col + SLOT_BITS;

	for (int y = 0; y < height; y++) {
		uint8_t       *drow = dst + y * ROW_BYTES;
		const uint8_t *srow = src + y * ROW_BYTES;

		for (int c = start_col; c < end_col; c++) {
			const uint8_t bit_mask = (uint8_t)(0x80U >> (c & 7));
			const int b = c >> 3;

			drow[b] = (uint8_t)((drow[b] & ~bit_mask) |
					    (srow[b] & bit_mask));
		}
	}
}

/* (Re)build a top inv variant for slot s. If @p strip is non-NULL, slot s
 * comes from the HLSS pressed strip; otherwise it is XOR-inverted from orig. */
static void build_top_variant(int s, const uint8_t *strip)
{
	uint8_t *dst = top_inv[s].buf + I1_PALETTE_BYTES;

	memcpy(dst, top_orig, TOP_BITMAP_BYTES);
	if (strip) {
		replace_slot(dst, strip, TOP_H, s);
	} else {
		invert_slot(dst, TOP_H, s);
	}
}

static void build_bot_variant(int s, const uint8_t *strip)
{
	uint8_t *dst = bot_inv[s].buf + I1_PALETTE_BYTES;

	memcpy(dst, bot_orig, BOT_BITMAP_BYTES);
	if (strip) {
		replace_slot(dst, strip, BOT_H, s);
	} else {
		invert_slot(dst, BOT_H, s);
	}
}

void press_feedback_capture_locked(const uint8_t *frame_bitmap)
{
	if (!frame_bitmap) {
		return;
	}

	memcpy(top_orig, frame_bitmap, TOP_BITMAP_BYTES);
	memcpy(bot_orig, frame_bitmap + (DISPLAY_H - BOT_H) * ROW_BYTES,
	       BOT_BITMAP_BYTES);

	/* Derive enabled-slot bitmasks from what HLSS actually drew. This is
	 * the fallback path: HLSS can override via the strip-id companion
	 * metadata once we plumb top_enabled_mask / bottom_enabled_mask
	 * through state/input responses. */
	const int all_bot_slots[SLOTS] = { 0, 1, 2, 3, 4, 5, 6, 7 };

	top_enabled_mask = scan_enabled_mask(top_orig, TOP_H,
					     top_usable_slots,
					     ARRAY_SIZE(top_usable_slots));
	bot_enabled_mask = scan_enabled_mask(bot_orig, BOT_H,
					     all_bot_slots, SLOTS);

	/* Default = INVERT variants; refresh_strips will upgrade any band
	 * whose pressed strip lands in the cache after prefetch. */
	for (size_t i = 0; i < ARRAY_SIZE(top_usable_slots); i++) {
		build_top_variant(top_usable_slots[i], NULL);
	}
	for (int s = 0; s < SLOTS; s++) {
		build_bot_variant(s, NULL);
	}

	captured = true;
}

void press_feedback_set_enabled_masks_locked(uint8_t top_mask, bool top_known,
					     uint8_t bot_mask, bool bot_known)
{
	if (top_known) {
		top_enabled_mask = top_mask;
	}
	if (bot_known) {
		bot_enabled_mask = bot_mask;
	}
}

void press_feedback_refresh_strips_locked(const char *top_strip_id,
					  const char *bottom_strip_id)
{
	if (!captured) {
		return;
	}

	size_t len = 0;

	const uint8_t *top_strip = (top_strip_id && top_strip_id[0])
		? llss_strip_cache_get(top_strip_id, &len)
		: NULL;
	if (top_strip && len == TOP_BITMAP_BYTES) {
		for (size_t i = 0; i < ARRAY_SIZE(top_usable_slots); i++) {
			build_top_variant(top_usable_slots[i], top_strip);
		}
	}

	const uint8_t *bot_strip = (bottom_strip_id && bottom_strip_id[0])
		? llss_strip_cache_get(bottom_strip_id, &len)
		: NULL;
	if (bot_strip && len == BOT_BITMAP_BYTES) {
		for (int s = 0; s < SLOTS; s++) {
			build_bot_variant(s, bot_strip);
		}
	}
}

void press_feedback_hide_locked(void)
{
	if (top_img) {
		lv_obj_add_flag(top_img, LV_OBJ_FLAG_HIDDEN);
	}
	if (bot_img) {
		lv_obj_add_flag(bot_img, LV_OBJ_FLAG_HIDDEN);
	}
}

void press_feedback_show(enum ui_btn btn)
{
	if (btn < 0 || btn >= UI_BTN_COUNT) {
		return;
	}
	const struct slot_pos *pos = &slot_table[btn];

	if (pos->band == BAND_NONE) {
		return;
	}
	if (!top_img || !bot_img || !captured) {
		return;
	}

	/* Device-local UI (menu, screensaver) consumes the press itself; the
	 * server-frame slots aren't what the user is looking at, so don't
	 * flash one. */
	if (device_ui_is_active()) {
		return;
	}

	const bool is_top    = (pos->band == BAND_TOP);
	const uint8_t mask   = is_top ? top_enabled_mask : bot_enabled_mask;

	/* Disabled / empty slot — nothing to highlight. Skipping here also
	 * prevents the invert fallback from turning an empty slot into a
	 * black rectangle. */
	if (!(mask & (uint8_t)(1U << pos->slot))) {
		return;
	}

	lv_obj_t *band_img   = is_top ? top_img : bot_img;
	lv_image_dsc_t *src  = is_top ? &top_inv[pos->slot].dsc
				      : &bot_inv[pos->slot].dsc;

	k_mutex_lock(&lvgl_mutex, K_FOREVER);

	press_feedback_hide_locked();

	lv_image_set_src(band_img, src);
	lv_obj_invalidate(band_img);
	lv_obj_clear_flag(band_img, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(band_img);

	ui_lvgl_flush(false, UI_CTX_SERVER);

	k_mutex_unlock(&lvgl_mutex);
}
