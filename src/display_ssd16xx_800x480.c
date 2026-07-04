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
#define SSD16XX_CMD_WRITE_TEMP            0x1A

#if CONFIG_LLSS_DISPLAY_USE_CUSTOM_FULL_LUT
/*
 * Custom mode-2 GC (full-clean) waveform for THIS unit. The panel's OTP
 * mode-1 banks under-drive rows whose source data flips against the previous
 * gate line (fine vertical detail washes out — see docs/epd/WAVEFORMS.md,
 * 2026-07-04 investigation); every mode-2 waveform, including the old
 * borrowed 3.7" LUT, renders the same content correctly. So the full refresh
 * is a hand-built mode-2 waveform at the proven frame rate (FR nibble 0x2),
 * driven with 0x22=0xCF exactly like the old LUT.
 *
 * Row indexing (0x21=0x00, both RAM planes = new image, level=(RED<<1)|BW):
 *   L0 = black pixels, L3 = white pixels, L1/L2 unused (zero), L4 = VCOM (DC).
 * VS phase codes: 00=VSS, 01=VSH1 (toward black), 10=VSL (toward white).
 *
 * Shape: both rows share a synchronized inversion flash — group 0 =
 * (white 8, black 8, white 8, black 8) x2 = 64 frames — which is what
 * actually erases ghost; then group 1 sets the target hard for 20 frames
 * (the old LUT set whites for only 4 frames — the "grayish white" defect);
 * group 2 discharges at VSS. Per update: black row nets +20 black frames,
 * white row +20 white — balanced across the black<->white alternation of
 * real content, VCOM stays DC throughout.
 */
static const uint8_t lut_gc_mode2[105] = {
	/* VS: 10 group-bytes per row, 4 phases (2 bits) per byte     */
	0x99, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L0 black: flash, set VSH1 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L1 unused */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L2 unused */
	0x99, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L3 white: flash, set VSL */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L4 VCOM: DC */
	/* TP/RP: {tpA, tpB, tpC, tpD, repeat} per group              */
	0x08, 0x08, 0x08, 0x08, 0x01,                                /* G0: flash x2 = 64 frames */
	0x14, 0x00, 0x00, 0x00, 0x00,                                /* G1: 20-frame target set  */
	0x02, 0x00, 0x00, 0x00, 0x00,                                /* G2: 2-frame VSS settle   */
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	/* Frame rate: one nibble per group; 0x2 = the old LUT's proven rate */
	0x22, 0x22, 0x22, 0x22, 0x22,
};
#endif

/*
 * Retention-grade partial ("deep partial"), mode 2, transition-indexed:
 * row = (old<<1)|new with BW RAM = new frame, RED RAM = previous frame —
 * the exact plane layout do_partial already writes. Unchanged pixels
 * (rows 00/11) are not driven at all — no flash, no flicker; changed pixels
 * get a short opposite kick then a 20-frame saturating set, deep enough to
 * be bistable across power-off (the OTP DU partial's shallow drive is why
 * partial-updated pixels fade). ~40 frames, changed-area flicker only.
 * For the deep-sleep clock path: digits update without a full-screen flash
 * yet retain like a GC-driven image.
 */
static const uint8_t lut_du_deep[105] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L0 b->b: hold */
	0x66, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L1 b->w: mini-flash, set VSL */
	0x99, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L2 w->b: mini-flash, set VSH1 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L3 w->w: hold */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L4 VCOM: DC */
	/* Retention needs full-range excursions before the set (a long
	 * monotonic push alone fades after power-off — glass-tested): two
	 * local inversion cycles, then the same 20f set depth as the GC. */
	0x08, 0x08, 0x08, 0x08, 0x00,                                /* G0: 2 inversion cycles  */
	0x14, 0x00, 0x00, 0x00, 0x00,                                /* G1: 20f saturating set  */
	0x02, 0x00, 0x00, 0x00, 0x00,                                /* G2: 2f VSS settle       */
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x22, 0x22, 0x22, 0x22, 0x22,
};

/*
 * Incremental grayscale (crosspoint/freeink mechanism), mode 2, level-coded:
 * runs OVER an already-displayed BW page. Pixels coded 00/11 in the planes
 * are untouched (zero rows); code 01 is pulled from white to light gray
 * (3 black frames), code 10 to dark gray (7). The revert LUT drives the same
 * codes back to white (slightly over-length to clear fully) before the next
 * BW update. Both passes leave the panel's prev-frame semantics broken, so
 * the driver invalidates prev_valid — the next normal refresh self-promotes
 * to full unless the caller restores the BW baseline first.
 */
static const uint8_t lut_gray_enhance[105] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L0: untouched */
	/* Stable grays need the slow FR2 frames (high-FR micro-pulses sit at
	 * the surface and relax away — glass-tested). Glass calibration at
	 * FR2, one-way: 3f = dark gray, 7f = near black; so light=2f,
	 * dark=4f. G0 = light row's dose, G1 = dark row's — independent. */
	0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L1 light: BLK 1 */
	0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L2 dark:  BLK 2 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L3: untouched */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L4 VCOM: DC */
	0x01, 0x00, 0x00, 0x00, 0x00,                                /* G0: light dose */
	0x02, 0x00, 0x00, 0x00, 0x00,                                /* G1: dark dose */
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x22, 0x22, 0x22, 0x22, 0x22,
};

static const uint8_t lut_gray_revert[105] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L0: untouched */
	/* Clearing a gray needs the same trick in reverse: kick INTO black,
	 * then drive white long — a one-way white push leaves residue. */
	0x20, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L1: WHT 12 + WHT 8 */
	0x60, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L2: BLK 2, WHT 12 + WHT 8 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L3: untouched */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* L4 VCOM: DC */
	0x02, 0x0C, 0x00, 0x00, 0x00,                                /* G0: kick + white set */
	0x08, 0x00, 0x00, 0x00, 0x00,                                /* G1: extra white */
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x22, 0x22, 0x22, 0x22, 0x22,
};

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

	/* Telemetry for the `epd status` shell command / refresh tuning. */
	uint32_t last_refresh_ms;
	uint8_t last_seq;
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

/* Write both framebuffer planes into the controller RAM (BW 0x24 + RED 0x26).
 * For mono content the planes are mirrored; for 4-gray they carry the 2bpp
 * split. This matches the vendor Display_Base() path, which also seeds RED as
 * the diff baseline for subsequent partials.
 */
static int custom_ssd16xx_write_planes(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;
	int32_t err;

	err = custom_ssd16xx_reset_ram_ptr(dev);
	if (err < 0) {
		return err;
	}

	LOG_DBG("Writing BW plane to EPD");
	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_WRITE_BW_RAM, data->bw_plane, CUSTOM_SSD16XX_BUF_BYTES);
	if (err < 0) {
		return err;
	}

	err = custom_ssd16xx_reset_ram_ptr(dev);
	if (err < 0) {
		return err;
	}

	LOG_DBG("Writing RED plane to EPD");
	return custom_ssd16xx_write_raw(dev, SSD16XX_CMD_WRITE_RED_RAM, data->red_plane, CUSTOM_SSD16XX_BUF_BYTES);
}

/* Flush both planes to panel and trigger a full refresh. Renders both
 * mono and 2-gray content correctly (mono frames have BW == RED mirrored).
 */
static int custom_ssd16xx_do_full(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;
	uint8_t tmp[2];
	int32_t err;

	err = custom_ssd16xx_write_planes(dev);
	if (err < 0) {
		return err;
	}

	/* Restore the full-refresh border (a preceding partial set 0x80;
	 * leaving that active gives every later full a dark edge ring). */
	tmp[0] = 0x01;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_BORDER_WAVEFORM, tmp, 1);
	if (err < 0) {
		return err;
	}

#if CONFIG_LLSS_DISPLAY_USE_CUSTOM_FULL_LUT
	LOG_DBG("Loading custom mode-2 GC LUT");
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_WRITE_LUT, lut_gc_mode2, sizeof(lut_gc_mode2));
	if (err < 0) {
		return err;
	}

	/* Display Update Control 2: 0xCF = custom LUT, display mode 2 */
	tmp[0] = 0xCF;
#else
	/* Display Update Control 2: 0xF7 = load real temperature + factory OTP
	 * GC waveform, display, power down. The ghost eraser (vendor full). */
	tmp[0] = 0xF7;
#endif
	data->last_seq = tmp[0];
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_DISP_UPDATE_CTRL2, tmp, 1);
	if (err < 0) {
		return err;
	}

	/* Master Activation */
	uint32_t t0 = k_uptime_get_32();

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
	data->last_refresh_ms = k_uptime_get_32() - t0;
	LOG_DBG("EPD refresh complete (%u ms)", data->last_refresh_ms);

	custom_ssd16xx_mark_synced(data);
	return 0;
}

/*
 * Fast-full refresh: fake a hot temperature (0x1A) so 0x22=0xD7 (load OTP LUT
 * for the *written* temp, skip the sensor read) replays a shorter factory
 * waveform bank — the vendor's "Fast(1.5s)" mode. Fully-driven and
 * ghost-clearing like a full, just from a hotter shelf. The next 0xF7/0xFC
 * reload the real temperature, so the fake value doesn't leak.
 */
static int custom_ssd16xx_do_fast(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;
	uint8_t tmp[1];
	int32_t err;

	err = custom_ssd16xx_write_planes(dev);
	if (err < 0) {
		return err;
	}

	tmp[0] = 0x01; /* full-refresh border (see do_full) */
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_BORDER_WAVEFORM, tmp, 1);
	if (err < 0) {
		return err;
	}

	tmp[0] = CONFIG_LLSS_EPD_FAST_FULL_TEMP;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_WRITE_TEMP, tmp, 1);
	if (err < 0) {
		return err;
	}

	tmp[0] = 0xD7;
	data->last_seq = tmp[0];
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_DISP_UPDATE_CTRL2, tmp, 1);
	if (err < 0) {
		return err;
	}

	uint32_t t0 = k_uptime_get_32();

	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_MASTER_ACTIVATION, NULL, 0);
	if (err < 0) {
		return err;
	}

	err = custom_ssd16xx_busy_wait(dev, CUSTOM_SSD16XX_REFRESH_TIMEOUT_MS);
	if (err < 0) {
		LOG_ERR("EPD fast-full refresh timed out");
		return err;
	}
	data->last_refresh_ms = k_uptime_get_32() - t0;
	LOG_DBG("EPD fast-full complete (%u ms)", data->last_refresh_ms);

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
	data->last_seq = tmp[0];
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

	uint32_t t0 = k_uptime_get_32();

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
	data->last_refresh_ms = k_uptime_get_32() - t0;

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

int custom_ssd16xx_refresh_fast(const struct device *dev)
{
	return custom_ssd16xx_do_fast(dev);
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

/* Load a custom LUT and run a mode-2 update (0x22=0xCF) on the RAM planes
 * as previously written by the caller. Shared tail of the deep-partial and
 * gray enhance/revert paths. */
static int custom_ssd16xx_run_lut(const struct device *dev, const uint8_t *lut)
{
	struct custom_ssd16xx_data *data = dev->data;
	uint8_t tmp[1];
	int32_t err;

	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_WRITE_LUT, lut, 105);
	if (err < 0) {
		return err;
	}

	tmp[0] = 0xCF;
	data->last_seq = tmp[0];
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_DISP_UPDATE_CTRL2, tmp, 1);
	if (err < 0) {
		return err;
	}

	uint32_t t0 = k_uptime_get_32();

	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_MASTER_ACTIVATION, NULL, 0);
	if (err < 0) {
		return err;
	}

	err = custom_ssd16xx_busy_wait(dev, CUSTOM_SSD16XX_REFRESH_TIMEOUT_MS);
	if (err < 0) {
		LOG_ERR("EPD custom-LUT update timed out");
		return err;
	}
	data->last_refresh_ms = k_uptime_get_32() - t0;
	return 0;
}

int custom_ssd16xx_refresh_partial_deep(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;
	uint8_t tmp[2];
	int32_t err;

	/* Same preconditions as the fast partial (see refresh_partial). */
	if (data->color_mode != CUSTOM_SSD16XX_MONO || !data->prev_valid ||
	    (data->full_refresh_interval != 0U &&
	     data->partial_count >= data->full_refresh_interval)) {
		return custom_ssd16xx_do_full(dev);
	}

	/* Previous frame -> RED RAM, new frame -> BW RAM (transition index). */
	err = custom_ssd16xx_reset_ram_ptr(dev);
	if (err < 0) {
		return err;
	}
	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_WRITE_RED_RAM, data->prev_bw_plane,
				       CUSTOM_SSD16XX_BUF_BYTES);
	if (err < 0) {
		return err;
	}
	err = custom_ssd16xx_reset_ram_ptr(dev);
	if (err < 0) {
		return err;
	}
	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_WRITE_BW_RAM, data->bw_plane,
				       CUSTOM_SSD16XX_BUF_BYTES);
	if (err < 0) {
		return err;
	}

	tmp[0] = 0x80; /* partial border */
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_BORDER_WAVEFORM, tmp, 1);
	if (err < 0) {
		return err;
	}

	err = custom_ssd16xx_run_lut(dev, lut_du_deep);
	if (err < 0) {
		return err;
	}

	/* Same post-refresh RED re-sync as the fast partial. */
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
	LOG_DBG("EPD deep partial complete (%u ms, %u since full)",
		data->last_refresh_ms, data->partial_count);
	return 0;
}

int custom_ssd16xx_refresh_gray(const struct device *dev, bool revert)
{
	struct custom_ssd16xx_data *data = dev->data;
	int32_t err;

	/* The planes hold 2bpp gray codes, not the displayed BW image: write
	 * both to RAM and run the enhance/revert waveform over the page. */
	err = custom_ssd16xx_write_planes(dev);
	if (err < 0) {
		return err;
	}

	err = custom_ssd16xx_run_lut(dev,
				     revert ? lut_gray_revert : lut_gray_enhance);
	if (err < 0) {
		return err;
	}

	/* Glass no longer matches the mono shadow: force the next normal
	 * refresh to be a full unless the caller restores the BW baseline. */
	data->prev_valid = false;
	return 0;
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

#ifdef CONFIG_LLSS_EPD_TEST_SHELL
/* Set one pixel at a 2bpp level (0=black..3=white) in both planes. */
static inline void test_set_px(struct custom_ssd16xx_data *data,
			       uint16_t px, uint16_t py, uint8_t level)
{
	uint16_t byte_idx = py * CUSTOM_SSD16XX_COLS + px / 8u;
	uint8_t bit = BIT(7u - (px % 8u));

	if (level & 0x01u) {
		data->bw_plane[byte_idx] |= bit;
	} else {
		data->bw_plane[byte_idx] &= ~bit;
	}
	if (level & 0x02u) {
		data->red_plane[byte_idx] |= bit;
	} else {
		data->red_plane[byte_idx] &= ~bit;
	}
}

int custom_ssd16xx_test_fill(const struct device *dev,
			     enum custom_ssd16xx_test_pattern pattern)
{
	struct custom_ssd16xx_data *data = dev->data;

	switch (pattern) {
	case CUSTOM_SSD16XX_PAT_WHITE:
		memset(data->bw_plane, 0xFF, CUSTOM_SSD16XX_BUF_BYTES);
		memset(data->red_plane, 0xFF, CUSTOM_SSD16XX_BUF_BYTES);
		break;
	case CUSTOM_SSD16XX_PAT_BLACK:
		memset(data->bw_plane, 0x00, CUSTOM_SSD16XX_BUF_BYTES);
		memset(data->red_plane, 0x00, CUSTOM_SSD16XX_BUF_BYTES);
		break;
	case CUSTOM_SSD16XX_PAT_CHECKER:
		for (uint16_t py = 0; py < CUSTOM_SSD16XX_ROWS; py++) {
			for (uint16_t px = 0; px < 800; px++) {
				bool black = ((px / 64u) + (py / 64u)) & 1u;

				test_set_px(data, px, py, black ? 0u : 3u);
			}
		}
		break;
	case CUSTOM_SSD16XX_PAT_DIGITS:
		/* White field with 4 large solid blocks — reproduces the
		 * screensaver big-digit ghosting scenario. */
		memset(data->bw_plane, 0xFF, CUSTOM_SSD16XX_BUF_BYTES);
		memset(data->red_plane, 0xFF, CUSTOM_SSD16XX_BUF_BYTES);
		for (uint16_t py = 100; py < 380; py++) {
			for (uint8_t blk = 0; blk < 4; blk++) {
				uint16_t x0 = 72u + blk * 184u;

				for (uint16_t px = x0; px < x0 + 120u; px++) {
					test_set_px(data, px, py, 0u);
				}
			}
		}
		break;
	case CUSTOM_SSD16XX_PAT_DITHER1:
	case CUSTOM_SSD16XX_PAT_DITHER2:
		/* Fine checkerboard ≈ a fully dithered mid-gray field: every
		 * source line toggles on (nearly) every gate line — worst-case
		 * booster/source load, mimicking the dithered chess board. */
		{
			uint16_t shift = (pattern == CUSTOM_SSD16XX_PAT_DITHER2) ? 1u : 0u;

			for (uint16_t py = 0; py < CUSTOM_SSD16XX_ROWS; py++) {
				for (uint16_t px = 0; px < 800; px++) {
					bool black = (((px >> shift) + (py >> shift)) & 1u) != 0u;

					test_set_px(data, px, py, black ? 0u : 3u);
				}
			}
		}
		break;
	case CUSTOM_SSD16XX_PAT_HLINES:
	case CUSTOM_SSD16XX_PAT_VLINES:
		/* Thin isolated lines: crisp when gate/data alignment is right,
		 * gray/doubled/displaced when the waveform smears along an axis.
		 * hlines exposes vertical smear, vlines horizontal. */
		{
			bool horiz = (pattern == CUSTOM_SSD16XX_PAT_HLINES);

			for (uint16_t py = 0; py < CUSTOM_SSD16XX_ROWS; py++) {
				for (uint16_t px = 0; px < 800; px++) {
					bool black = ((horiz ? py : px) % 8u) == 0u;

					test_set_px(data, px, py, black ? 0u : 3u);
				}
			}
		}
		break;
	case CUSTOM_SSD16XX_PAT_GRADIENT:
		/* 4 vertical bands at 2bpp levels 0..3. Under a mono waveform
		 * the BW plane reads black/white/black/white — which also makes
		 * plane-role/polarity mixups visible at a glance. */
		for (uint16_t py = 0; py < CUSTOM_SSD16XX_ROWS; py++) {
			for (uint16_t px = 0; px < 800; px++) {
				test_set_px(data, px, py, px / 200u);
			}
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int custom_ssd16xx_test_sequence(const struct device *dev, uint8_t seq,
				 int16_t border, int16_t temp,
				 enum custom_ssd16xx_test_ram ram)
{
	struct custom_ssd16xx_data *data = dev->data;
	uint8_t tmp[1];
	int32_t err;

	if (ram != CUSTOM_SSD16XX_RAM_NONE) {
		err = custom_ssd16xx_reset_ram_ptr(dev);
		if (err < 0) {
			return err;
		}
		err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_WRITE_BW_RAM,
					       data->bw_plane, CUSTOM_SSD16XX_BUF_BYTES);
		if (err < 0) {
			return err;
		}
	}
	if (ram == CUSTOM_SSD16XX_RAM_BOTH) {
		err = custom_ssd16xx_reset_ram_ptr(dev);
		if (err < 0) {
			return err;
		}
		err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_WRITE_RED_RAM,
					       data->red_plane, CUSTOM_SSD16XX_BUF_BYTES);
		if (err < 0) {
			return err;
		}
	}

	if (temp >= 0) {
		tmp[0] = (uint8_t)temp;
		err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_WRITE_TEMP, tmp, 1);
		if (err < 0) {
			return err;
		}
	}

	if (border >= 0) {
		tmp[0] = (uint8_t)border;
		err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_BORDER_WAVEFORM, tmp, 1);
		if (err < 0) {
			return err;
		}
	}

	tmp[0] = seq;
	data->last_seq = seq;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_DISP_UPDATE_CTRL2, tmp, 1);
	if (err < 0) {
		return err;
	}

	uint32_t t0 = k_uptime_get_32();

	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_MASTER_ACTIVATION, NULL, 0);
	if (err < 0) {
		return err;
	}

	err = custom_ssd16xx_busy_wait(dev, CUSTOM_SSD16XX_REFRESH_TIMEOUT_MS);
	if (err < 0) {
		LOG_ERR("EPD test sequence 0x%02X timed out", seq);
		return err;
	}
	data->last_refresh_ms = k_uptime_get_32() - t0;
	LOG_INF("EPD seq 0x%02X done in %u ms", seq, data->last_refresh_ms);

	/* Keep the pipeline's diff baseline consistent with what's on glass. */
	custom_ssd16xx_mark_synced(data);
	return 0;
}

int custom_ssd16xx_test_compare(const struct device *dev,
				struct custom_ssd16xx_plane_cmp *out)
{
	struct custom_ssd16xx_data *data = dev->data;
	uint64_t bw_white = 0, red_white = 0;

	out->diff_bytes = 0;
	out->first_diff = UINT32_MAX;
	out->prev_diff_bytes = 0;

	for (uint32_t i = 0; i < CUSTOM_SSD16XX_BUF_BYTES; i++) {
		if (data->bw_plane[i] != data->red_plane[i]) {
			out->diff_bytes++;
			if (out->first_diff == UINT32_MAX) {
				out->first_diff = i;
			}
		}
		if (data->bw_plane[i] != data->prev_bw_plane[i]) {
			out->prev_diff_bytes++;
		}
		bw_white += POPCOUNT(data->bw_plane[i]);
		red_white += POPCOUNT(data->red_plane[i]);
	}

	uint64_t total = (uint64_t)CUSTOM_SSD16XX_BUF_BYTES * 8u;

	out->bw_ink_pct = (uint32_t)(((total - bw_white) * 100u) / total);
	out->red_ink_pct = (uint32_t)(((total - red_white) * 100u) / total);
	return 0;
}

int custom_ssd16xx_test_reg(const struct device *dev, uint8_t cmd,
			    const uint8_t *data, size_t len)
{
	return custom_ssd16xx_write_cmd(dev, cmd, len ? data : NULL, len);
}

static uint8_t test_snap_buf[CUSTOM_SSD16XX_BUF_BYTES]
	__attribute__((section(".ext_ram.bss")));
static bool test_snap_valid;

int custom_ssd16xx_test_snap(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;

	memcpy(test_snap_buf, data->bw_plane, CUSTOM_SSD16XX_BUF_BYTES);
	test_snap_valid = true;
	return 0;
}

int custom_ssd16xx_test_restore(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;

	if (!test_snap_valid) {
		return -ENOENT;
	}
	memcpy(data->bw_plane, test_snap_buf, CUSTOM_SSD16XX_BUF_BYTES);
	memcpy(data->red_plane, test_snap_buf, CUSTOM_SSD16XX_BUF_BYTES);
	return 0;
}

int custom_ssd16xx_test_row(const struct device *dev, uint16_t row,
			    uint8_t out[100])
{
	struct custom_ssd16xx_data *data = dev->data;

	if (row >= CUSTOM_SSD16XX_ROWS) {
		return -EINVAL;
	}
	memcpy(out, &data->bw_plane[(size_t)row * CUSTOM_SSD16XX_COLS],
	       CUSTOM_SSD16XX_COLS);
	return 0;
}

int custom_ssd16xx_get_status(const struct device *dev,
			      struct custom_ssd16xx_status *status)
{
	struct custom_ssd16xx_data *data = dev->data;

	status->partial_count = data->partial_count;
	status->full_refresh_interval = data->full_refresh_interval;
	status->prev_valid = data->prev_valid;
	status->color_mode = (uint8_t)data->color_mode;
	status->last_refresh_ms = data->last_refresh_ms;
	status->last_seq = data->last_seq;
	return 0;
}
#endif /* CONFIG_LLSS_EPD_TEST_SHELL */

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