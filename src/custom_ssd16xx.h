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

/*
 * Fast 1bpp partial refresh (~600ms). Falls back to a full refresh when:
 *   - color mode is not MONO (gray cannot partial),
 *   - no valid previous frame exists yet (first frame after boot/mode change),
 *   - the periodic full-refresh interval has been reached (ghosting floor).
 */
int custom_ssd16xx_refresh_partial(const struct device *dev);

/* Declare the color mode of the framebuffer. A change forces the next refresh
 * to be full (panel waveform state must be reset across a mode transition).
 */
int custom_ssd16xx_set_color_mode(const struct device *dev,
				  enum custom_ssd16xx_color_mode mode);

/*
 * Enable/disable ordered (Bayer 4x4) dithering during the L8 -> 2bpp
 * conversion in write().
 *
 * OFF (default): nearest-band quantisation (luma >> 6).  Correct for content
 * the server already dithered to the panel's exact levels (0/85/170/255) —
 * re-dithering would corrupt it.
 *
 * ON: device-local UI screens, which use arbitrary grays.  Dithering renders
 * those as believable shades on the 4-level panel instead of hard banding.
 */
int custom_ssd16xx_set_dither(const struct device *dev, bool enable);

#endif /* CUSTOM_SSD16XX_H */
