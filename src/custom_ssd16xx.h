#ifndef CUSTOM_SSD16XX_H
#define CUSTOM_SSD16XX_H

#include <zephyr/device.h>

/*
 * Color mode of the data currently in the framebuffer. Gray2 (2bpp) and
 * the fast 1bpp partial refresh are mutually exclusive on this panel, so the
 * mode selects which refresh waveform is valid. The app declares the mode of
 * the frame it just wrote; the driver picks the refresh accordingly.
 */
enum custom_ssd16xx_color_mode {
	CUSTOM_SSD16XX_MONO = 0, /* 1bpp, partial-refresh capable */
	CUSTOM_SSD16XX_GRAY2,    /* 2bpp 4-gray, full refresh only */
};

/* Full GC refresh (~2s). Renders both mono and 2-gray content correctly and
 * resets the panel's internal "previous frame" to the current framebuffer.
 */
int custom_ssd16xx_refresh_full(const struct device *dev);

/* Fast-full refresh: factory OTP waveform from a hotter temperature bank
 * (0x1A=CONFIG_LLSS_EPD_FAST_FULL_TEMP + 0x22=0xD7). Fully driven and
 * ghost-clearing like a full, somewhat quicker; for user-visible content
 * changes (move confirmed, view toggle) where a GC full feels too slow.
 */
int custom_ssd16xx_refresh_fast(const struct device *dev);

/*
 * Fast 1bpp partial refresh (~600ms). Falls back to a full refresh when:
 *   - color mode is not MONO (gray cannot partial),
 *   - no valid previous frame exists yet (first frame after boot/mode change),
 *   - the periodic full-refresh interval has been reached (ghosting floor).
 */
int custom_ssd16xx_refresh_partial(const struct device *dev);

/* Default automatic full-refresh floor: number of consecutive partials after
 * which custom_ssd16xx_refresh_partial() forces a full refresh to clear
 * accumulated ghosting.
 */
#define CUSTOM_SSD16XX_DEFAULT_FULL_REFRESH_INTERVAL 20U

/*
 * Override the automatic full-refresh floor at runtime.
 *
 * `interval` == 0 DISABLES the auto-floor entirely: partial refreshes never
 * self-promote to full, leaving the application in complete control of when
 * fulls happen (via custom_ssd16xx_refresh_full() / UI_CTX_SWITCH). Used by
 * the screensaver, which schedules its own flashes (e.g. on the wall-clock
 * half-hour) and must not be interrupted by the floor. Any non-zero value sets
 * a new floor; CUSTOM_SSD16XX_DEFAULT_FULL_REFRESH_INTERVAL restores the
 * default. The change takes effect on the next refresh.
 */
int custom_ssd16xx_set_full_refresh_interval(const struct device *dev,
					     uint32_t interval);

/* Declare the color mode of the framebuffer. A change forces the next refresh
 * to be full (panel waveform state must be reset across a mode transition).
 */
int custom_ssd16xx_set_color_mode(const struct device *dev,
				  enum custom_ssd16xx_color_mode mode);

/*
 * Enable/disable ordered (Bayer 4x4) dithering in write().
 *
 * In MONO mode (the partial-capable default), dithering quantises arbitrary
 * grays DOWN to 1bpp black/white: the apparent shade comes from the dot
 * pattern, not a gray waveform, so dithered content stays partial-refresh
 * capable.
 *
 * OFF (default): hard threshold.  Correct for content already dithered to
 * pure B/W by the server (its 0/255 must be preserved, not re-dithered).
 *
 * ON: device-local UI screens, which use arbitrary grays (anti-aliased fonts,
 * gray fills) and want believable shades instead of hard banding.
 *
 * (In GRAY2 mode this flag instead dithers across the 4 gray levels; GRAY2 is
 * full-refresh only.)
 */
int custom_ssd16xx_set_dither(const struct device *dev, bool enable);

#ifdef CONFIG_LLSS_EPD_TEST_SHELL
/*
 * Waveform test harness (shell `epd` commands). Serialize with the display
 * pipeline: callers must hold lvgl_mutex (see display_thread.h).
 */

enum custom_ssd16xx_test_pattern {
	CUSTOM_SSD16XX_PAT_WHITE = 0,
	CUSTOM_SSD16XX_PAT_BLACK,
	CUSTOM_SSD16XX_PAT_CHECKER,  /* 64x64 px squares */
	CUSTOM_SSD16XX_PAT_DIGITS,   /* 4 big solid blocks (screensaver-stain scenario) */
	CUSTOM_SSD16XX_PAT_GRADIENT, /* 4 vertical bands at 2bpp levels 0..3 */
	CUSTOM_SSD16XX_PAT_DITHER1,  /* 1px checkerboard: worst-case drive load */
	CUSTOM_SSD16XX_PAT_DITHER2,  /* 2px checkerboard: server-dither-like load */
	CUSTOM_SSD16XX_PAT_HLINES,   /* 1px black rows every 8: fine VERTICAL detail */
	CUSTOM_SSD16XX_PAT_VLINES,   /* 1px black cols every 8: fine HORIZONTAL detail */
};

/* Which controller RAM planes custom_ssd16xx_test_sequence() rewrites before
 * triggering the update. NONE replays against whatever the controller holds.
 */
enum custom_ssd16xx_test_ram {
	CUSTOM_SSD16XX_RAM_BOTH = 0, /* BW=bw_plane, RED=red_plane (our full path) */
	CUSTOM_SSD16XX_RAM_BW,       /* BW only (vendor EPD_3IN97_Display path) */
	CUSTOM_SSD16XX_RAM_NONE,
};

struct custom_ssd16xx_status {
	uint32_t partial_count;
	uint32_t full_refresh_interval;
	bool prev_valid;
	uint8_t color_mode;
	uint32_t last_refresh_ms;
	uint8_t last_seq; /* last 0x22 value driven */
};

/* Fill the framebuffer planes with a test pattern (does not touch the panel;
 * follow with refresh_full/refresh_partial/test_sequence). */
int custom_ssd16xx_test_fill(const struct device *dev,
			     enum custom_ssd16xx_test_pattern pattern);

/*
 * Raw update-sequence experiment: optionally rewrite RAM planes, optionally
 * write the temperature register (0x1A, -1 skips) and border waveform (0x3C,
 * -1 skips), then run 0x22=<seq> + master activation and busy-wait. Marks the
 * frame synced so the normal pipeline stays consistent afterwards.
 */
int custom_ssd16xx_test_sequence(const struct device *dev, uint8_t seq,
				 int16_t border, int16_t temp,
				 enum custom_ssd16xx_test_ram ram);

int custom_ssd16xx_get_status(const struct device *dev,
			      struct custom_ssd16xx_status *status);

struct custom_ssd16xx_plane_cmp {
	uint32_t diff_bytes;   /* bytes where BW and RED planes differ */
	uint32_t first_diff;   /* offset of first differing byte (or UINT32_MAX) */
	uint32_t bw_ink_pct;   /* % of black pixels in the BW plane */
	uint32_t red_ink_pct;  /* % of black pixels in the RED plane */
	uint32_t prev_diff_bytes; /* bytes where BW differs from prev (shadow) */
};

/* Diff the framebuffer planes against each other and the prev shadow —
 * diagnoses "full mangled / partial fine" (a full displays RED, a partial
 * ignores it: divergence means someone wrote one plane and not the other). */
int custom_ssd16xx_test_compare(const struct device *dev,
				struct custom_ssd16xx_plane_cmp *out);

/* Raw register write (`epd reg`): send an arbitrary command + data bytes.
 * For experiments only — e.g. 0x21 (Display Update Control 1) RED-bypass. */
int custom_ssd16xx_test_reg(const struct device *dev, uint8_t cmd,
			    const uint8_t *data, size_t len);

/* Snapshot the BW plane into a spare buffer / restore it into both planes.
 * Lets a captured real-world frame be replayed through arbitrary sequences
 * after synthetic patterns overwrote the framebuffer. */
int custom_ssd16xx_test_snap(const struct device *dev);
int custom_ssd16xx_test_restore(const struct device *dev);

/* Copy one row (100 bytes) of the BW plane into out. */
int custom_ssd16xx_test_row(const struct device *dev, uint16_t row,
			    uint8_t out[100]);
#endif /* CONFIG_LLSS_EPD_TEST_SHELL */

#endif /* CUSTOM_SSD16XX_H */
