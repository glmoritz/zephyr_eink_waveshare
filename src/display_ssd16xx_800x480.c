#define DT_DRV_COMPAT custom_ssd16xx_800x480

#include <stdint.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mipi_dbi.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "custom_ssd16xx.h"

LOG_MODULE_REGISTER(custom_ssd16xx_800x480, CONFIG_DISPLAY_LOG_LEVEL);

#define CUSTOM_SSD16XX_BUSY_TIMEOUT_MS    2000U
#define CUSTOM_SSD16XX_REFRESH_TIMEOUT_MS 15000U

/* SSD1677 / SSD1681-family command register addresses */
#define SSD16XX_CMD_SOFT_RESET            0x12
#define SSD16XX_CMD_AUTO_WRITE_RED_RAM    0x46
#define SSD16XX_CMD_AUTO_WRITE_BW_RAM     0x47
#define SSD16XX_CMD_DRIVER_OUTPUT_CTRL    0x01
#define SSD16XX_CMD_GATE_VOLTAGE          0x03
#define SSD16XX_CMD_SOURCE_VOLTAGE        0x04
#define SSD16XX_CMD_BOOSTER_SOFT_START    0x0C
#define SSD16XX_CMD_DATA_ENTRY_MODE       0x11
#define SSD16XX_CMD_BORDER_WAVEFORM       0x3C
#define SSD16XX_CMD_TEMP_SENSOR_CTRL      0x18
#define SSD16XX_CMD_VCOM_VOLTAGE          0x2C
#define SSD16XX_CMD_DISPLAY_OPTION        0x37
#define SSD16XX_CMD_WRITE_LUT             0x32
#define SSD16XX_CMD_RAM_X_WINDOW          0x44
#define SSD16XX_CMD_RAM_Y_WINDOW          0x45
#define SSD16XX_CMD_RAM_X_COUNTER         0x4E
#define SSD16XX_CMD_RAM_Y_COUNTER         0x4F
#define SSD16XX_CMD_WRITE_BW_RAM          0x24
#define SSD16XX_CMD_WRITE_RED_RAM         0x26
#define SSD16XX_CMD_DISP_UPDATE_CTRL1     0x21
#define SSD16XX_CMD_DISP_UPDATE_CTRL2     0x22
#define SSD16XX_CMD_MASTER_ACTIVATION     0x20
#define SSD16XX_CMD_DEEP_SLEEP            0x10

#if CONFIG_LLSS_DISPLAY_USE_CUSTOM_FULL_LUT
/*
 * 4-gray waveform LUT from the Waveshare 3.7" SSD16xx-family panel.
 * Same controller family as our 3.97" panel. Send via SSD16XX_CMD_WRITE_LUT (105 bytes).
 */
static const uint8_t lut_4gray_gc[105] = {
	0x2A, 0x06, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 1 */
	0x28, 0x06, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 2 */
	0x20, 0x06, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 3 */
	0x14, 0x06, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 4 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 5 */
	0x00, 0x02, 0x02, 0x0A, 0x00, 0x00, 0x00, 0x08, 0x08, 0x02, /* 6 */
	0x00, 0x02, 0x02, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 7 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 8 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 9 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 10 */
	0x22, 0x22, 0x22, 0x22, 0x22,                               /* gate */
};
#endif

/* Fixed geometry for this 800x480 driver */
#define CUSTOM_SSD16XX_COLS      (800U / 8U)  /* 100 bytes per row */
#define CUSTOM_SSD16XX_ROWS      480U
#define CUSTOM_SSD16XX_BUF_BYTES (CUSTOM_SSD16XX_COLS * CUSTOM_SSD16XX_ROWS) /* 48000 */

/* Force a full refresh after this many consecutive partial refreshes, to clear
 * accumulated ghosting. The driver owns this floor; the app may request a full
 * refresh earlier (e.g. HLSS switch). Tune for the deployed panel/temperature.
 */
#ifndef CUSTOM_SSD16XX_FULL_REFRESH_INTERVAL
#define CUSTOM_SSD16XX_FULL_REFRESH_INTERVAL CUSTOM_SSD16XX_DEFAULT_FULL_REFRESH_INTERVAL
#endif

struct custom_ssd16xx_config {
	const struct device *mipi_dev;
	struct mipi_dbi_config dbi_config;
	struct gpio_dt_spec busy_gpio;
	uint16_t width;
	uint16_t height;
};

struct custom_ssd16xx_data {
	bool blanking_on;
	/* 4-gray (2bpp): bit1 goes to BW RAM (0x24), bit0 goes to RED RAM (0x26)
	 * Actual buffers live in SPIRAM (.ext_ram.bss), declared per-instance below.
	 */
	uint8_t *bw_plane;
	uint8_t *red_plane;

	/* Shadow of the last frame driven to the panel, used as the "previous"
	 * image the controller diffs against during a 1bpp partial refresh.
	 */
	uint8_t *prev_bw_plane;
	bool prev_valid;
	enum custom_ssd16xx_color_mode color_mode;
	uint32_t partial_count;
	/* Auto full-refresh floor: force a full once partial_count reaches this.
	 * 0 disables the floor (app owns fulls — see set_full_refresh_interval). */
	uint32_t full_refresh_interval;

	/* Ordered dithering during L8->2bpp conversion (see header). Off for
	 * pre-dithered server frames, on for device-local UI. */
	bool dither;
};

static int custom_ssd16xx_busy_wait(const struct device *dev, uint32_t timeout_ms)
{
	const struct custom_ssd16xx_config *config = dev->config;
	int32_t pin = gpio_pin_get_dt(&config->busy_gpio);
	uint32_t elapsed = 0;

	if (pin < 0) {
		LOG_ERR("Failed to read BUSY GPIO (%d)", pin);
		return pin;
	}

	while (pin > 0) {
		if (elapsed >= timeout_ms) {
			LOG_ERR("EPD BUSY timeout after %u ms", timeout_ms);
			return -ETIMEDOUT;
		}

		k_msleep(5);
		elapsed += 5;
		pin = gpio_pin_get_dt(&config->busy_gpio);
		if (pin < 0) {
			LOG_ERR("Failed to read BUSY GPIO (%d)", pin);
			return pin;
		}
	}

	return 0;
}

/* Wait for BUSY, then send command + data */
static int custom_ssd16xx_write_cmd(const struct device *dev, uint8_t cmd,
					const uint8_t *data, size_t len)
{
	const struct custom_ssd16xx_config *config = dev->config;
	int32_t err;

	err = custom_ssd16xx_busy_wait(dev, CUSTOM_SSD16XX_BUSY_TIMEOUT_MS);
	if (err < 0) {
		return err;
	}

	err = mipi_dbi_command_write(config->mipi_dev, &config->dbi_config, cmd, data, len);
	mipi_dbi_release(config->mipi_dev, &config->dbi_config);
	return err;
}

/* Send command + data WITHOUT a busy-wait (for bulk pixel streaming) */
static int custom_ssd16xx_write_raw(const struct device *dev, uint8_t cmd,
					const uint8_t *data, size_t len)
{
	const struct custom_ssd16xx_config *config = dev->config;
	int32_t err = mipi_dbi_command_write(config->mipi_dev, &config->dbi_config, cmd, data, len);

	mipi_dbi_release(config->mipi_dev, &config->dbi_config);
	return err;
}

static int custom_ssd16xx_panel_init(const struct device *dev)
{
	const struct custom_ssd16xx_config *config = dev->config;
	uint8_t data[8];
	int32_t err;

	err = mipi_dbi_reset(config->mipi_dev, 10);
	if (err < 0) {
		return err;
	}

	k_msleep(10);
	err = custom_ssd16xx_busy_wait(dev, CUSTOM_SSD16XX_BUSY_TIMEOUT_MS);
	if (err < 0) {
		return err;
	}

	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_SOFT_RESET, NULL, 0);
	if (err < 0) {
		return err;
	}

	data[0] = 0x80;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_TEMP_SENSOR_CTRL, data, 1);
	if (err < 0) {
		return err;
	}

	data[0] = 0xAE;
	data[1] = 0xC7;
	data[2] = 0xC3;
	data[3] = 0xC0;
	data[4] = 0x80;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_BOOSTER_SOFT_START, data, 5);
	if (err < 0) {
		return err;
	}

	data[0] = (config->height - 1) & 0xFF;
	data[1] = ((config->height - 1) >> 8) & 0xFF;
	data[2] = 0x02;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_DRIVER_OUTPUT_CTRL, data, 3);
	if (err < 0) {
		return err;
	}

	data[0] = 0x01;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_BORDER_WAVEFORM, data, 1);
	if (err < 0) {
		return err;
	}

	data[0] = 0x01;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_DATA_ENTRY_MODE, data, 1);
	if (err < 0) {
		return err;
	}

	data[0] = 0x00;
	data[1] = 0x00;
	data[2] = (config->width - 1) & 0xFF;
	data[3] = ((config->width - 1) >> 8) & 0xFF;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_RAM_X_WINDOW, data, 4);
	if (err < 0) {
		return err;
	}

	data[0] = (config->height - 1) & 0xFF;
	data[1] = ((config->height - 1) >> 8) & 0xFF;
	data[2] = 0x00;
	data[3] = 0x00;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_RAM_Y_WINDOW, data, 4);
	if (err < 0) {
		return err;
	}

	data[0] = 0x00;
	data[1] = 0x00;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_RAM_X_COUNTER, data, 2);
	if (err < 0) {
		return err;
	}

	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_RAM_Y_COUNTER, data, 2);
	if (err < 0) {
		return err;
	}

	return custom_ssd16xx_busy_wait(dev, CUSTOM_SSD16XX_BUSY_TIMEOUT_MS);
}

static int custom_ssd16xx_blanking_on(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;

	data->blanking_on = true;
	return 0;
}

/* Reset the RAM address pointers to the top-left origin before a plane write.
 * X resets to 0; Y resets to height-1 (Y decrements from top to bottom).
 */
static int custom_ssd16xx_reset_ram_ptr(const struct device *dev)
{
	uint8_t tmp[2];
	int32_t err;

	tmp[0] = 0;
	tmp[1] = 0;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_RAM_X_COUNTER, tmp, 2);
	if (err < 0) {
		return err;
	}

	tmp[0] = (CUSTOM_SSD16XX_ROWS - 1) & 0xFF;
	tmp[1] = ((CUSTOM_SSD16XX_ROWS - 1) >> 8) & 0x01;
	return custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_RAM_Y_COUNTER, tmp, 2);
}

/* Mark the framebuffer as the new "previous" frame after a successful refresh. */
static void custom_ssd16xx_mark_synced(struct custom_ssd16xx_data *data)
{
	memcpy(data->prev_bw_plane, data->bw_plane, CUSTOM_SSD16XX_BUF_BYTES);
	data->prev_valid = true;
	data->partial_count = 0;
}

/* Flush both planes to panel and trigger a full 4-gray refresh. Renders both
 * mono and 2-gray content correctly (mono frames have BW == RED mirrored).
 */
static int custom_ssd16xx_do_full(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;
	uint8_t tmp[2];
	int32_t err;

	err = custom_ssd16xx_reset_ram_ptr(dev);
	if (err < 0) {
		return err;
	}

	/* Write MSB plane (bit1 of 2bpp) into BW RAM */
	LOG_DBG("Writing BW plane to EPD");
	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_WRITE_BW_RAM, data->bw_plane, CUSTOM_SSD16XX_BUF_BYTES);
	if (err < 0) {
		return err;
	}

	err = custom_ssd16xx_reset_ram_ptr(dev);
	if (err < 0) {
		return err;
	}

	/* Write LSB plane (bit0 of 2bpp) into RED RAM */
	LOG_DBG("Writing RED plane to EPD");
	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_WRITE_RED_RAM, data->red_plane, CUSTOM_SSD16XX_BUF_BYTES);
	if (err < 0) {
		return err;
	}

#if CONFIG_LLSS_DISPLAY_USE_CUSTOM_FULL_LUT
	LOG_DBG("Loading custom 4-gray LUT");
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_WRITE_LUT, lut_4gray_gc, sizeof(lut_4gray_gc));
	if (err < 0) {
		return err;
	}

	/* Display Update Control 2: 0xCF = custom LUT full refresh */
	tmp[0] = 0xCF;
#else
	/* Display Update Control 2: 0xF7 = controller OTP/integrated waveform */
	tmp[0] = 0xF7;
#endif
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_DISP_UPDATE_CTRL2, tmp, 1);
	if (err < 0) {
		return err;
	}

	/* Master Activation */
	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_MASTER_ACTIVATION, NULL, 0);
	if (err < 0) {
		return err;
	}

	LOG_DBG("Waiting for EPD refresh...");
	err = custom_ssd16xx_busy_wait(dev, CUSTOM_SSD16XX_REFRESH_TIMEOUT_MS);
	if (err < 0) {
		LOG_ERR("EPD refresh timed out");
		return err;
	}
	LOG_DBG("EPD refresh complete");

	custom_ssd16xx_mark_synced(data);
	return 0;
}

/*
 * 1bpp differential partial refresh using the OTP partial waveform.
 * The controller diffs the new frame (BW RAM) against the previous frame
 * (RED RAM) and only drives changed pixels (~600ms vs ~2s full).
 */
static int custom_ssd16xx_do_partial(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;
	uint8_t tmp[2];
	int32_t err;

	/* Previous frame -> RED RAM (the image the controller diffs against) */
	err = custom_ssd16xx_reset_ram_ptr(dev);
	if (err < 0) {
		return err;
	}
	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_WRITE_RED_RAM, data->prev_bw_plane,
				       CUSTOM_SSD16XX_BUF_BYTES);
	if (err < 0) {
		return err;
	}

	/* New frame -> BW RAM */
	err = custom_ssd16xx_reset_ram_ptr(dev);
	if (err < 0) {
		return err;
	}
	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_WRITE_BW_RAM, data->bw_plane,
				       CUSTOM_SSD16XX_BUF_BYTES);
	if (err < 0) {
		return err;
	}

	/* Display Update Control 2: 0xFC = OTP partial waveform. (Option B if
	 * ghosting is bad: load lut_1Gray_A2 via 0x32 and use 0xC7 here instead.)
	 */
	tmp[0] = 0xFC;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_DISP_UPDATE_CTRL2, tmp, 1);
	if (err < 0) {
		return err;
	}

	tmp[0] = 0x00;
	tmp[1] = 0x00;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_DISP_UPDATE_CTRL1, tmp, 2);
	if (err < 0) {
		return err;
	}

	/* Partial border waveform */
	tmp[0] = 0x80;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_BORDER_WAVEFORM, tmp, 1);
	if (err < 0) {
		return err;
	}

	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_MASTER_ACTIVATION, NULL, 0);
	if (err < 0) {
		return err;
	}

	LOG_DBG("Waiting for EPD partial refresh...");
	err = custom_ssd16xx_busy_wait(dev, CUSTOM_SSD16XX_REFRESH_TIMEOUT_MS);
	if (err < 0) {
		LOG_ERR("EPD partial refresh timed out");
		return err;
	}

	/* Re-sync: write the new frame into RED RAM too, so the controller's
	 * internal "previous" matches reality for the next partial. Most
	 * implementations miss this and accumulate diffing errors.
	 */
	err = custom_ssd16xx_reset_ram_ptr(dev);
	if (err < 0) {
		return err;
	}
	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_WRITE_RED_RAM, data->bw_plane,
				       CUSTOM_SSD16XX_BUF_BYTES);
	if (err < 0) {
		return err;
	}

	memcpy(data->prev_bw_plane, data->bw_plane, CUSTOM_SSD16XX_BUF_BYTES);
	data->partial_count++;
	LOG_DBG("EPD partial refresh complete (%u since full)", data->partial_count);
	return 0;
}

/* Flush both planes to panel and trigger a full 4-gray refresh */
static int custom_ssd16xx_blanking_off(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;

	data->blanking_on = false;
	return custom_ssd16xx_do_full(dev);
}

int custom_ssd16xx_refresh_full(const struct device *dev)
{
	return custom_ssd16xx_do_full(dev);
}

int custom_ssd16xx_refresh_partial(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;

	/* Gray and partial are mutually exclusive on this panel; no previous
	 * frame means nothing to diff against; the periodic floor clears
	 * ghosting. Any of these falls back to a full refresh.
	 */
	if (data->color_mode != CUSTOM_SSD16XX_MONO || !data->prev_valid ||
	    (data->full_refresh_interval != 0U &&
	     data->partial_count >= data->full_refresh_interval)) {
		return custom_ssd16xx_do_full(dev);
	}

	return custom_ssd16xx_do_partial(dev);
}

int custom_ssd16xx_set_full_refresh_interval(const struct device *dev,
					     uint32_t interval)
{
	struct custom_ssd16xx_data *data = dev->data;

	data->full_refresh_interval = interval;
	return 0;
}

int custom_ssd16xx_set_color_mode(const struct device *dev,
				  enum custom_ssd16xx_color_mode mode)
{
	struct custom_ssd16xx_data *data = dev->data;

	if (mode != data->color_mode) {
		/* A mode transition resets the panel waveform state: force the
		 * next refresh to be full by invalidating the previous frame.
		 */
		data->color_mode = mode;
		data->prev_valid = false;
	}
	return 0;
}

/* 4x4 ordered (Bayer) dither matrix, values 0..15. Indexed by absolute screen
 * coordinates so the pattern is stable across LVGL's chunked region writes. */
static const uint8_t bayer4[4][4] = {
	{  0,  8,  2, 10 },
	{ 12,  4, 14,  6 },
	{  3, 11,  1,  9 },
	{ 15,  7, 13,  5 },
};

/*
 * Quantise one 8-bit luma to a 2bpp gray level (0=black .. 3=white).
 *
 * Plain mode: nearest band (luma >> 6) — palette-aligned for pre-dithered
 * server frames (0/85/170/255 land exactly on levels 0/1/2/3).
 *
 * Dither mode: ordered dithering across the 4 levels using the Bayer matrix,
 * so arbitrary UI grays render as a mix of adjacent levels instead of banding.
 */
static inline uint8_t quantise_2bpp(uint8_t luma, bool dither,
				    uint16_t px, uint16_t py)
{
	if (!dither) {
		return luma >> 6;
	}

	int32_t scaled = (int32_t)luma * 3;          /* 0..765 = level*255 */
	int32_t base   = scaled / 255;               /* lower level 0..3 */
	int32_t frac   = scaled - base * 255;        /* 0..254 toward next level */
	int32_t thr    = bayer4[py & 3][px & 3] * 255 / 16; /* 0..239 */
	int32_t level  = base + (frac > thr ? 1 : 0);

	return (uint8_t)(level > 3 ? 3 : level);
}

/*
 * Convert an L_8 (8-bit grayscale) region into the 2-plane framebuffer.
 *
 * 2bpp level packing: BW bit = level&1, RED bit = (level>>1)&1, giving
 *   0 black, 1 dark gray, 2 light gray, 3 white.
 * Bit packing: MSB = leftmost pixel (bit 7 = column 0 within a byte).
 */
static int custom_ssd16xx_write(const struct device *dev, const uint16_t x,
				const uint16_t y,
				const struct display_buffer_descriptor *desc,
				const void *buf)
{
	struct custom_ssd16xx_data *data = dev->data;
	const uint8_t *src = buf;
	bool dither = data->dither;
	bool mono = (data->color_mode == CUSTOM_SSD16XX_MONO);

	for (uint16_t row = 0; row < desc->height; row++) {
		for (uint16_t col = 0; col < desc->width; col++) {
			uint8_t luma = src[(size_t)row * desc->pitch + col];
			uint16_t px = x + col;
			uint16_t py = y + row;
			uint8_t g2;

			if (mono) {
				/* 1bpp, partial-refresh capable. Arbitrary grays are
				 * ordered-dithered DOWN to pure B/W: the shade comes from
				 * the dot pattern, not a gray waveform, so dithered UI
				 * (e.g. the clock) stays partial-capable. Without dither,
				 * a hard threshold preserves an already-dithered frame's
				 * 0/255 instead of re-dithering it. */
				if (dither) {
					int32_t thr = bayer4[py & 3][px & 3] * 16 + 8; /* 8..248 */

					g2 = (luma > thr) ? 0x3u : 0x0u;
				} else {
					g2 = (luma >= 128u) ? 0x3u : 0x0u;
				}
			} else {
				/* GRAY2: real 4-level waveform path, full refresh only. */
				g2 = quantise_2bpp(luma, dither, px, py);
			}
			uint16_t byte_idx = py * CUSTOM_SSD16XX_COLS + px / 8u;
			uint8_t bit_pos = 7u - (px % 8u);

			if (g2 & 0x01u) {
				data->bw_plane[byte_idx] |= BIT(bit_pos);
			} else {
				data->bw_plane[byte_idx] &= ~BIT(bit_pos);
			}
			if (g2 & 0x02u) {
				data->red_plane[byte_idx] |= BIT(bit_pos);
			} else {
				data->red_plane[byte_idx] &= ~BIT(bit_pos);
			}
		}
	}
	return 0;
}

int custom_ssd16xx_set_dither(const struct device *dev, bool enable)
{
	struct custom_ssd16xx_data *data = dev->data;

	data->dither = enable;
	return 0;
}

static void custom_ssd16xx_get_capabilities(const struct device *dev,
					    struct display_capabilities *caps)
{
	const struct custom_ssd16xx_config *config = dev->config;

	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = config->width;
	caps->y_resolution = config->height;
	caps->supported_pixel_formats = PIXEL_FORMAT_L_8;
	caps->current_pixel_format = PIXEL_FORMAT_L_8;
	caps->screen_info = SCREEN_INFO_EPD;
}

static int custom_ssd16xx_set_pixel_format(const struct device *dev,
					   const enum display_pixel_format pf)
{
	ARG_UNUSED(dev);
	return pf == PIXEL_FORMAT_L_8 ? 0 : -ENOTSUP;
}

static int custom_ssd16xx_init(const struct device *dev)
{
	const struct custom_ssd16xx_config *config = dev->config;

	if (!device_is_ready(config->mipi_dev)) {
		LOG_ERR("MIPI DBI transport not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&config->busy_gpio)) {
		LOG_ERR("Busy GPIO not ready");
		return -ENODEV;
	}

	if (gpio_pin_configure_dt(&config->busy_gpio, GPIO_INPUT) < 0) {
		LOG_ERR("Failed to configure busy GPIO");
		return -EIO;
	}

	return custom_ssd16xx_panel_init(dev);
}

static DEVICE_API(display, custom_ssd16xx_api) = {
	.blanking_on = custom_ssd16xx_blanking_on,
	.blanking_off = custom_ssd16xx_blanking_off,
	.write = custom_ssd16xx_write,
	.get_capabilities = custom_ssd16xx_get_capabilities,
	.set_pixel_format = custom_ssd16xx_set_pixel_format,
};

#define CUSTOM_SSD16XX_DEFINE(inst) \
	/* 48 KB each — placed in SPIRAM to keep DRAM free */ \
	static uint8_t custom_ssd16xx_bw_##inst[CUSTOM_SSD16XX_BUF_BYTES] \
		__attribute__((section(".ext_ram.bss"))); \
	static uint8_t custom_ssd16xx_red_##inst[CUSTOM_SSD16XX_BUF_BYTES] \
		__attribute__((section(".ext_ram.bss"))); \
	static uint8_t custom_ssd16xx_prev_bw_##inst[CUSTOM_SSD16XX_BUF_BYTES] \
		__attribute__((section(".ext_ram.bss"))); \
	static const struct custom_ssd16xx_config custom_ssd16xx_cfg_##inst = { \
		.mipi_dev = DEVICE_DT_GET(DT_PARENT(DT_DRV_INST(inst))), \
		.dbi_config = { \
			.mode = MIPI_DBI_MODE_SPI_4WIRE, \
			.config = MIPI_DBI_SPI_CONFIG_DT(DT_DRV_INST(inst), \
				SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_HOLD_ON_CS | SPI_LOCK_ON, 0), \
		}, \
		.busy_gpio = GPIO_DT_SPEC_GET(DT_DRV_INST(inst), busy_gpios), \
		.width = DT_PROP(DT_DRV_INST(inst), width), \
		.height = DT_PROP(DT_DRV_INST(inst), height), \
	}; \
	static struct custom_ssd16xx_data custom_ssd16xx_data_##inst = { \
		.bw_plane  = custom_ssd16xx_bw_##inst, \
		.red_plane = custom_ssd16xx_red_##inst, \
		.prev_bw_plane = custom_ssd16xx_prev_bw_##inst, \
		.prev_valid = false, \
		.color_mode = CUSTOM_SSD16XX_MONO, \
		.partial_count = 0, \
		.full_refresh_interval = CUSTOM_SSD16XX_FULL_REFRESH_INTERVAL, \
	}; \
	DEVICE_DT_INST_DEFINE(inst, custom_ssd16xx_init, NULL, \
		&custom_ssd16xx_data_##inst, &custom_ssd16xx_cfg_##inst, \
		POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY, &custom_ssd16xx_api);

DT_INST_FOREACH_STATUS_OKAY(CUSTOM_SSD16XX_DEFINE)