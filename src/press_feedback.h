#ifndef PRESS_FEEDBACK_H_
#define PRESS_FEEDBACK_H_

#include <stdint.h>
#include <stddef.h>

#include <lvgl.h>

#include "input_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Local press-feedback overlays.
 *
 * One LVGL image per band, with N per-slot pre-computed variants. The variant
 * for slot S shows the band with slot S in pressed visual state and every
 * other slot unchanged. Press becomes a pointer swap.
 *
 * The slot-S pressed pixels come from one of two sources, decided at frame-
 * capture time and re-decided whenever HLSS strips arrive:
 *   - INVERT (default): the captured band's slot S, XOR-inverted.
 *   - HLSS pressed strip: a content-addressable strip cached via
 *     llss_strip_cache, holding the band rendered with every button pressed;
 *     we extract just slot S's column range into the variant.
 *
 * Show on finger-down. Hide on new server frame. No release event needed.
 */

void press_feedback_init(lv_obj_t *parent);

/* Capture the band areas of the just-submitted frame and (re-)pre-compute
 * every per-slot variant using the INVERT path (no strips yet). Also
 * derives a default enabled-slot bitmask by detecting all-white (no
 * button drawn) slots — overridden later by HLSS-supplied masks. Caller
 * must hold lvgl_mutex. */
void press_feedback_capture_locked(const uint8_t *frame_bitmap);

/* Re-pre-compute the per-slot variants using cached HLSS pressed strips
 * for the bands whose strip ids land in the cache. Bands with no cached
 * strip keep the INVERT-derived variants. Caller must hold lvgl_mutex. */
void press_feedback_refresh_strips_locked(const char *top_strip_id,
					  const char *bottom_strip_id);

/* Override the per-band enabled-slot bitmasks (bit S = slot S is
 * pressable). Pass 0xFF for "all 8 slots enabled", 0 for "none".
 * @p top_known / @p bot_known = false means "leave the heuristic-derived
 * mask alone for that band" — used when the server advertises no mask. */
void press_feedback_set_enabled_masks_locked(uint8_t top_mask, bool top_known,
					     uint8_t bot_mask, bool bot_known);

/* Show pressed feedback for @p btn. Takes lvgl_mutex internally and
 * triggers a partial-refresh flush. */
void press_feedback_show(enum ui_btn btn);

/* Hide overlays. Caller must hold lvgl_mutex. */
void press_feedback_hide_locked(void);

#ifdef __cplusplus
}
#endif

#endif /* PRESS_FEEDBACK_H_ */
