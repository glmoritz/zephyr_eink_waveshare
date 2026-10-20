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

#ifdef CONFIG_LLSS_EPD_FAST_DRIVE
/* Fast-drive engine geometry (see the "Fast drive" section below). A pixel's
 * remaining pulse budget is bit-sliced over FAST_CNT_PLANES planes (budget
 * <= 15); one uint32_t word covers 32 pixels of a row in every plane. */
#define FAST_CNT_PLANES    4U
#define FAST_WORDS         (CUSTOM_SSD16XX_BUF_BYTES / 4U)        /* 12000 */
#define FAST_WORDS_PER_ROW (CUSTOM_SSD16XX_COLS / 4U)             /* 25 */
#define FAST_ROW_WORDS     ((CUSTOM_SSD16XX_ROWS + 31U) / 32U)    /* 15 */

#define FAST_SEQ_SCAN      0x0C /* display, mode 2; rails left as they are */
#define FAST_SEQ_POWER_ON  0xC0 /* clock + analog on, no display */
#define FAST_SEQ_POWER_OFF 0x03 /* analog + clock off */

/* Glass (box black -> white): 8 white pulses leave a faint mark, 15 erase
 * it fully; 8 black pulses already give solid black. */
#define CUSTOM_SSD16XX_FAST_CFG_DEFAULT {                                   \
	.black_pulses = 8, .white_pulses = 15, .frames = 1, .fr = 0x2,      \
	.vss_frame = false, .hold_ending = true, .red_skip = true,          \
	.keep_power = true, .sync_scans = 1,                                \
}

/* Inclusive framebuffer row range; empty when y0 > y1. */
struct fast_rows {
	uint16_t y0;
	uint16_t y1;
};

struct custom_ssd16xx_fast {
	uint32_t *cnt[FAST_CNT_PLANES]; /* bit-sliced pulse budgets */
	uint32_t *red;                  /* RED RAM image: target ^ driving */
	struct custom_ssd16xx_fast_cfg cfg;
	bool enabled;       /* refresh_partial() routes through the engine */
	bool synced;        /* controller LUT/border/RAM belong to the engine */
	bool analog_on;
	bool hold_pending;  /* last drive spent, one all-HOLD scan owed */
	bool first_pending; /* the submit's first scan is not delivered yet */
	uint32_t own_updates; /* data->updates after our last activation */
	struct fast_rows act;               /* rows still holding budget */
	uint32_t red_rows[FAST_ROW_WORDS];  /* RED RAM rows to rewrite */
	uint32_t bw_rows[FAST_ROW_WORDS];   /* BW RAM rows to rewrite */
	uint32_t t_start;   /* k_cycle_get_32() at submit */
	struct custom_ssd16xx_fast_stats st;
	/* f->red mirrors the controller's RED RAM (false until the post-sync
	 * rewrite went out), so red_skip can drop rows that would not change. */
	bool red_known;
	/* Framebuffer rows written since the last submit — the only rows the
	 * submit has to diff. Every bw_plane writer extends it. */
	struct fast_rows fb_dirty;
};

static inline void fast_rows_clear(struct fast_rows *r)
{
	r->y0 = CUSTOM_SSD16XX_ROWS;
	r->y1 = 0;
}

static inline bool fast_rows_empty(const struct fast_rows *r)
{
	return r->y0 > r->y1;
}

static inline void fast_rows_add(struct fast_rows *r, uint16_t y)
{
	r->y0 = MIN(r->y0, y);
	r->y1 = MAX(r->y1, y);
}

static inline void fast_mark(uint32_t *bits, uint16_t y)
{
	bits[y / 32U] |= BIT(y % 32U);
}

static inline void fast_unmark(uint32_t *bits, uint16_t y)
{
	bits[y / 32U] &= ~BIT(y % 32U);
}

static inline bool fast_marked(const uint32_t *bits, uint16_t y)
{
	return (bits[y / 32U] & BIT(y % 32U)) != 0U;
}

static int custom_ssd16xx_fast_run(const struct device *dev, uint8_t max_scans);
static void custom_ssd16xx_fast_prewarm(const struct device *dev);
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

	/* Mono write() threshold; 128 default, 64 in the gray-enhance flow. */
	uint8_t mono_thr;

	/* Gray codes are on glass (enhance ran, revert hasn't). While set, a
	 * partial self-promotes to full; a full or revert clears it. */
	bool in_gray;

	/* Telemetry for the `epd status` shell command / refresh tuning. */
	uint32_t last_refresh_ms;
	uint8_t last_seq;
	/* Bumped by every master activation alongside last_seq, so the fast
	 * engine can tell another path drove the panel since its last scan. */
	uint32_t updates;

	/* Controller RAM Y window spans the whole panel. The fast engine
	 * narrows it; reset_ram_ptr() widens it back before full-plane writes. */
	bool ram_window_full;

	/* Active pixel format. MONO01 = native 1bpp (write() is a memcpy);
	 * L_8 keeps the dormant dither/GRAY2/GRAY_MARK conversion path alive. */
	enum display_pixel_format pixel_format;

#ifdef CONFIG_LLSS_EPD_FAST_DRIVE
	struct custom_ssd16xx_fast fast;
#endif
};

/* Record framebuffer rows [y, y+h) as written (fast-drive diff window). */
static inline void custom_ssd16xx_note_rows(struct custom_ssd16xx_data *data,
					    uint16_t y, uint16_t h)
{
#ifdef CONFIG_LLSS_EPD_FAST_DRIVE
	if (h == 0U || y >= CUSTOM_SSD16XX_ROWS) {
		return;
	}
	fast_rows_add(&data->fast.fb_dirty, y);
	fast_rows_add(&data->fast.fb_dirty,
		      (uint16_t)MIN((uint32_t)y + h - 1U, CUSTOM_SSD16XX_ROWS - 1U));
#else
	ARG_UNUSED(data);
	ARG_UNUSED(y);
	ARG_UNUSED(h);
#endif
}

/* TEMP instrumentation: cycles spent inside write() since the last refresh,
 * to separate our L8->plane conversion from LVGL's own rasterisation. */
static uint32_t g_write_cycles;

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
	struct custom_ssd16xx_data *data = dev->data;
	uint8_t tmp[4];
	int32_t err;

	/* The fast engine narrows the Y window to the rows it rewrites; every
	 * full-plane writer starts here, so widen it back first. */
	if (!data->ram_window_full) {
		tmp[0] = (CUSTOM_SSD16XX_ROWS - 1) & 0xFF;
		tmp[1] = ((CUSTOM_SSD16XX_ROWS - 1) >> 8) & 0x01;
		tmp[2] = 0x00;
		tmp[3] = 0x00;
		err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_RAM_Y_WINDOW, tmp, 4);
		if (err < 0) {
			return err;
		}
		data->ram_window_full = true;
	}

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
	data->updates++;
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

	data->in_gray = false;
	custom_ssd16xx_mark_synced(data);
#ifdef CONFIG_LLSS_EPD_FAST_DRIVE
	custom_ssd16xx_fast_prewarm(dev);
#endif
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
	data->updates++;
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
#ifdef CONFIG_LLSS_EPD_FAST_DRIVE
	custom_ssd16xx_fast_prewarm(dev);
#endif
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

	/* TEMP instrumentation (BTNTRACE): a partial clocks THREE 48 KB planes
	 * out over SPI, so separate that cost from the panel waveform. */
	uint32_t t_pre0 = k_uptime_get_32();

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

	uint32_t t_pre = k_uptime_get_32() - t_pre0;

	/* Display Update Control 2: 0xFC = OTP partial waveform. (Option B if
	 * ghosting is bad: load lut_1Gray_A2 via 0x32 and use 0xC7 here instead.)
	 */
	tmp[0] = 0xFC;
	data->last_seq = tmp[0];
	data->updates++;
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

	uint32_t t_post0 = k_uptime_get_32();

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

	uint32_t t_memcpy0 = k_uptime_get_32();

	memcpy(data->prev_bw_plane, data->bw_plane, CUSTOM_SSD16XX_BUF_BYTES);

	/* TEMP instrumentation: pre/post = SPI plane writes (2 then 1), wave =
	 * panel waveform (yields on k_msleep(5)), cpy = the 48 KB prev-plane
	 * memcpy (pure CPU, no yield). */
	LOG_INF("BTNTRACE epd partial pre=%u wave=%u post=%u cpy=%u wr_us=%u",
		t_pre, data->last_refresh_ms, t_memcpy0 - t_post0,
		k_uptime_get_32() - t_memcpy0,
		k_cyc_to_us_floor32(g_write_cycles));
	g_write_cycles = 0;

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
	 * frame means nothing to diff against; gray codes on glass break the
	 * diff premise; the periodic floor clears ghosting. Any of these
	 * falls back to a full refresh.
	 */
	if (data->color_mode == CUSTOM_SSD16XX_GRAY2 || !data->prev_valid ||
	    data->in_gray ||
	    (data->full_refresh_interval != 0U &&
	     data->partial_count >= data->full_refresh_interval)) {
		return custom_ssd16xx_do_full(dev);
	}

#ifdef CONFIG_LLSS_EPD_FAST_DRIVE
	if (data->fast.enabled) {
		int err = custom_ssd16xx_fast_run(dev, data->fast.cfg.sync_scans);
		const struct custom_ssd16xx_fast_stats *st = &data->fast.st;

		/* TEMP instrumentation (BTNTRACE), same role as do_partial's.
		 * total/scans cover only the synchronous part. */
		LOG_INF("BTNTRACE epd fast first=%u total=%u scans=%u plan=%u "
			"spi=%u wave=%u rows=%u..%u (us)",
			st->first_us, st->total_us, st->scans, st->plan_us,
			st->spi_us, st->wave_us, st->y0, st->y1);
		return err;
	}
#endif

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
	data->updates++;
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
	    data->in_gray ||
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

	if (revert && !data->in_gray) {
		return 0; /* nothing enhanced — no-op */
	}
	if (!revert && !data->prev_valid) {
		/* Enhance rides on top of a known BW baseline. */
		return -EINVAL;
	}

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

	if (revert) {
		/* Coded pixels are white again == their baseline value:
		 * restore planes so planes == glass == prev, partial-ready. */
		memcpy(data->bw_plane, data->prev_bw_plane, CUSTOM_SSD16XX_BUF_BYTES);
		memcpy(data->red_plane, data->prev_bw_plane, CUSTOM_SSD16XX_BUF_BYTES);
		data->in_gray = false;
	} else {
		data->in_gray = true;
	}
	return 0;
}

#ifdef CONFIG_LLSS_EPD_FAST_DRIVE
/* =========================================================================
 * Fast drive — per-pixel pulse engine, ported from kuslota/inkboy's SSD1681
 * "mixed scan" (itself after zephray/paperboy's per-pixel driving).
 *
 * Instead of one canned multi-frame waveform per update, the panel is driven
 * by SCANS: activations of a one-frame custom LUT (mode 2, row =
 * (RED<<1)|BW), repeated. Per pixel the RAM planes select the row:
 *   BW  = target image T (prev_bw_plane: what the pixel is heading to)
 *   RED = T ^ A, A = "pixel still has pulse budget"
 * so A=0 lands on rows 00/11 = HOLD (VSS, untouched); A=1 with T white on
 * row 01 = one VSL frame (toward white); A=1 with T black on row 10 = one
 * VSH1 frame (toward black). Same transition indexing as lut_du_deep.
 *
 * A pixel whose target changes gets a budget of N scans for its direction;
 * every delivered scan spends one pulse of every pixel it drove. A pixel
 * that changes again mid-budget simply restarts — no waiting for an earlier
 * waveform to finish. The analog rails stay up between scans (0xC0 once,
 * then 0x22=0x0C per scan), and RAM writes are Y-windowed to the rows whose
 * target or drive mask changed, so a scan costs about one panel frame.
 *
 * After the last pulse, one all-HOLD scan writes VSS into the storage caps
 * of the pixels driven last (inkboy's "HOLD ending"): with the rails up and
 * the gates idle, those caps would otherwise keep pushing on stored charge.
 *
 * The engine owns the controller between scans. Any other refresh path
 * bumps data->updates; the next fast call sees it, drops leftover budgets
 * (that path already settled the image) and re-enters: LUT reload + every
 * RAM row rewritten, since stale BW != RED pairs would be driven.
 * ========================================================================= */

/* 0x37 A..J all zero: spare-VCOM/display-mode bits at reset value, F[6]
 * (RAM ping-pong for mode 2) off. */
static const uint8_t fast_display_option[10];

static void fast_build_lut(const struct custom_ssd16xx_fast_cfg *cfg,
			   uint8_t lut[105])
{
	memset(lut, 0, 105);
	lut[10] = 0x80;                      /* L1 (R=0,BW=1) phase A: VSL  */
	lut[20] = 0x40;                      /* L2 (R=1,BW=0) phase A: VSH1 */
	lut[50] = cfg->frames;               /* G0 TP A: drive frames */
	lut[51] = cfg->vss_frame ? 1U : 0U;  /* G0 TP B: VS 00 = VSS, all rows */
	memset(&lut[100], (cfg->fr << 4) | cfg->fr, 5);
}

/* Write framebuffer rows [y0, y1] of one plane through a Y window (entry
 * mode 0x01: X+, Y-; framebuffer row r is gate address ROWS-1-r). */
static int fast_write_rows(const struct device *dev, uint8_t cmd,
			   const uint8_t *plane, uint16_t y0, uint16_t y1)
{
	struct custom_ssd16xx_data *data = dev->data;
	uint16_t ys = CUSTOM_SSD16XX_ROWS - 1U - y0;
	uint16_t ye = CUSTOM_SSD16XX_ROWS - 1U - y1;
	uint8_t tmp[4];
	int32_t err;

	tmp[0] = ys & 0xFF;
	tmp[1] = (ys >> 8) & 0x01;
	tmp[2] = ye & 0xFF;
	tmp[3] = (ye >> 8) & 0x01;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_RAM_Y_WINDOW, tmp, 4);
	if (err < 0) {
		return err;
	}
	data->ram_window_full = false;

	tmp[0] = 0;
	tmp[1] = 0;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_RAM_X_COUNTER, tmp, 2);
	if (err < 0) {
		return err;
	}
	tmp[0] = ys & 0xFF;
	tmp[1] = (ys >> 8) & 0x01;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_RAM_Y_COUNTER, tmp, 2);
	if (err < 0) {
		return err;
	}

	return custom_ssd16xx_write_raw(dev, cmd,
					plane + (size_t)y0 * CUSTOM_SSD16XX_COLS,
					(size_t)(y1 - y0 + 1U) * CUSTOM_SSD16XX_COLS);
}

/* Push every marked row of one plane, one window per contiguous run. The
 * marks are cleared only once all runs went out. */
static int fast_write_marked(const struct device *dev, uint8_t cmd,
			     const uint8_t *plane, uint32_t *bits)
{
	struct custom_ssd16xx_data *data = dev->data;
	uint16_t y = 0;

	while (y < CUSTOM_SSD16XX_ROWS) {
		if (!fast_marked(bits, y)) {
			y++;
			continue;
		}

		uint16_t y0 = y;

		while (y < CUSTOM_SSD16XX_ROWS && fast_marked(bits, y)) {
			y++;
		}

		int err = fast_write_rows(dev, cmd, plane, y0, y - 1U);

		if (err < 0) {
			return err;
		}
		data->fast.st.rows += y - y0;
	}
	memset(bits, 0, FAST_ROW_WORDS * sizeof(uint32_t));
	return 0;
}

/* 0x22=<seq> + master activation, then wait for BUSY with 100 us
 * resolution (the shared busy_wait polls every 5 ms — a quarter of a scan).
 * BUSY rises microseconds after 0x20; wait for that edge first so a late
 * rise can't read as "already done". */
static int fast_activate(const struct device *dev, uint8_t seq, uint32_t *us)
{
	const struct custom_ssd16xx_config *config = dev->config;
	struct custom_ssd16xx_data *data = dev->data;
	uint32_t t0, spins = 0;
	int32_t err, pin;

	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_DISP_UPDATE_CTRL2, &seq, 1);
	if (err < 0) {
		return err;
	}
	data->last_seq = seq;
	data->updates++;
	data->fast.own_updates = data->updates;

	uint32_t c0 = k_cycle_get_32();

	err = custom_ssd16xx_write_raw(dev, SSD16XX_CMD_MASTER_ACTIVATION, NULL, 0);
	if (err < 0) {
		return err;
	}

	while ((pin = gpio_pin_get_dt(&config->busy_gpio)) == 0) {
		if (++spins > 200U) {
			/* No rise within ~2 ms: nothing to do (e.g. rails
			 * already in the requested state). */
			if (seq == FAST_SEQ_SCAN) {
				data->fast.st.busy_miss++;
			}
			break;
		}
		k_busy_wait(10);
	}

	t0 = k_uptime_get_32();
	while (pin > 0) {
		if (k_uptime_get_32() - t0 > CUSTOM_SSD16XX_REFRESH_TIMEOUT_MS) {
			LOG_ERR("EPD fast 0x%02X: BUSY timeout", seq);
			return -ETIMEDOUT;
		}
		k_usleep(100);
		pin = gpio_pin_get_dt(&config->busy_gpio);
	}
	if (pin < 0) {
		LOG_ERR("Failed to read BUSY GPIO (%d)", pin);
		return pin;
	}

	*us = k_cyc_to_us_floor32(k_cycle_get_32() - c0);
	return 0;
}

/* Another refresh path activated the panel since our last scan: its LUT and
 * RAM layout replaced ours, and it settled the image to the framebuffer —
 * drop leftover budgets and re-enter on the next scan. */
static void fast_check_foreign(struct custom_ssd16xx_data *data)
{
	struct custom_ssd16xx_fast *f = &data->fast;

	if (data->updates == f->own_updates) {
		return;
	}
	f->own_updates = data->updates;
	f->synced = false;
	/* 0xFC (OTP partial) leaves the rails up; the full/LUT paths drop them. */
	f->analog_on = (data->last_seq & 0x40U) && !(data->last_seq & 0x02U);

	if (!fast_rows_empty(&f->act)) {
		size_t off = (size_t)f->act.y0 * FAST_WORDS_PER_ROW;
		size_t len = (size_t)(f->act.y1 - f->act.y0 + 1U) * CUSTOM_SSD16XX_COLS;

		for (size_t i = 0; i < FAST_CNT_PLANES; i++) {
			memset(&f->cnt[i][off], 0, len);
		}
	}
	fast_rows_clear(&f->act);
	f->hold_pending = false;
	f->first_pending = false;
}

/* Session entry: normal RAM roles, no border drive, our LUT; then every RAM
 * row is rewritten — whatever the last path left (gray codes, a test's
 * planes) would be driven by this LUT wherever BW != RED. */
static int fast_sync(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;
	struct custom_ssd16xx_fast *f = &data->fast;
	uint8_t lut[105];
	uint8_t tmp[2];
	int32_t err;

	tmp[0] = 0x00;
	tmp[1] = 0x00;
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_DISP_UPDATE_CTRL1, tmp, 2);
	if (err < 0) {
		return err;
	}

	tmp[0] = 0x80; /* partial border */
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_BORDER_WAVEFORM, tmp, 1);
	if (err < 0) {
		return err;
	}

	/* Display option F[6] = 0: no RAM ping-pong in mode 2. This panel comes
	 * up with it on (the vendor partial writes only 0x24 and relies on it),
	 * and then scans after the first don't see the planes as written: a
	 * box flickers faintly and ends white. Off, it drives solid black
	 * (glass-tested). The scans need RAM exactly as written — inkboy does
	 * the same. */
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_DISPLAY_OPTION,
				       fast_display_option,
				       sizeof(fast_display_option));
	if (err < 0) {
		return err;
	}

	fast_build_lut(&f->cfg, lut);
	err = custom_ssd16xx_write_cmd(dev, SSD16XX_CMD_WRITE_LUT, lut, sizeof(lut));
	if (err < 0) {
		return err;
	}

	memset(f->red_rows, 0xFF, sizeof(f->red_rows));
	memset(f->bw_rows, 0xFF, sizeof(f->bw_rows));
	f->red_known = false;
	f->synced = true;
	return 0;
}

/* RED image for every marked row: T ^ A, A read from the budgets. With
 * red_skip, a row whose image matches what the RAM already holds is
 * unmarked: e.g. a fresh change's first scan needs RED = old image, which
 * is exactly what the previous drive left there. */
static void fast_build_red(struct custom_ssd16xx_data *data)
{
	struct custom_ssd16xx_fast *f = &data->fast;
	const uint32_t *tw = (const uint32_t *)data->prev_bw_plane;
	bool cmp = f->cfg.red_skip && f->red_known;

	for (uint16_t y = 0; y < CUSTOM_SSD16XX_ROWS; y++) {
		if (!fast_marked(f->red_rows, y)) {
			continue;
		}

		size_t base = (size_t)y * FAST_WORDS_PER_ROW;
		bool live = (y >= f->act.y0 && y <= f->act.y1);
		bool same = true;

		for (size_t w = base; w < base + FAST_WORDS_PER_ROW; w++) {
			uint32_t a = 0;

			if (live) {
				for (size_t i = 0; i < FAST_CNT_PLANES; i++) {
					a |= f->cnt[i][w];
				}
			}

			uint32_t r = tw[w] ^ a;

			same = same && (f->red[w] == r);
			f->red[w] = r;
		}
		if (cmp && same) {
			fast_unmark(f->red_rows, y);
		}
	}
}

/* A scan was delivered: every driving pixel spent one pulse (bit-sliced
 * subtract-one, borrow rippling through the planes). Rows whose drive mask
 * changed get their RED row rewritten before the next scan. */
static void fast_decrement(struct custom_ssd16xx_data *data)
{
	struct custom_ssd16xx_fast *f = &data->fast;
	struct fast_rows live;

	fast_rows_clear(&live);
	for (uint16_t y = f->act.y0; y <= f->act.y1; y++) {
		size_t base = (size_t)y * FAST_WORDS_PER_ROW;
		bool row_live = false;
		bool row_changed = false;

		for (size_t w = base; w < base + FAST_WORDS_PER_ROW; w++) {
			uint32_t a = 0;

			for (size_t i = 0; i < FAST_CNT_PLANES; i++) {
				a |= f->cnt[i][w];
			}
			if (a == 0U) {
				continue;
			}

			uint32_t borrow = a;
			uint32_t na = 0;

			for (size_t i = 0; i < FAST_CNT_PLANES; i++) {
				uint32_t p = f->cnt[i][w];
				uint32_t np = p ^ borrow;

				borrow &= ~p;
				f->cnt[i][w] = np;
				na |= np;
			}
			row_changed |= (na != a);
			row_live |= (na != 0U);
		}
		if (row_changed) {
			fast_mark(f->red_rows, y);
		}
		if (row_live) {
			fast_rows_add(&live, y);
		}
	}
	f->act = live;
}

/* Rails up + controller RAM matching the engine (no activation). */
static int fast_prepare(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;
	struct custom_ssd16xx_fast *f = &data->fast;
	uint32_t c0 = k_cycle_get_32();
	uint32_t us;
	int32_t err;

	if (!f->synced) {
		err = fast_sync(dev);
		if (err < 0) {
			return err;
		}
		f->st.sync_us += k_cyc_to_us_floor32(k_cycle_get_32() - c0);
	}
	if (!f->analog_on) {
		err = fast_activate(dev, FAST_SEQ_POWER_ON, &us);
		if (err < 0) {
			return err;
		}
		f->analog_on = true;
		f->st.power_us += us;
	}

	c0 = k_cycle_get_32();
	if (!f->cfg.red_skip) {
		for (uint16_t y = f->act.y0; y <= f->act.y1; y++) {
			fast_mark(f->red_rows, y);
		}
	}
	fast_build_red(data);

	uint32_t c1 = k_cycle_get_32();

	f->st.calc_us += k_cyc_to_us_floor32(c1 - c0);

	err = fast_write_marked(dev, SSD16XX_CMD_WRITE_BW_RAM,
				data->prev_bw_plane, f->bw_rows);
	if (err < 0) {
		return err;
	}
	err = fast_write_marked(dev, SSD16XX_CMD_WRITE_RED_RAM,
				(const uint8_t *)f->red, f->red_rows);
	if (err < 0) {
		return err;
	}
	f->red_known = true;
	f->st.spi_us += k_cyc_to_us_floor32(k_cycle_get_32() - c1);
	return 0;
}

/* A full just dropped the rails. In fast mode, re-enter now — rails up, LUT
 * and RAM resynced (~83 ms + ~34 ms) — while nobody waits, instead of on the
 * next press. Stats keep describing the last submit. */
static void custom_ssd16xx_fast_prewarm(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;
	struct custom_ssd16xx_fast *f = &data->fast;

	if (!f->enabled || !f->cfg.keep_power) {
		return;
	}

	struct custom_ssd16xx_fast_stats keep = f->st;

	fast_check_foreign(data);
	if (fast_prepare(dev) < 0) {
		f->synced = false; /* retried by the next scan */
	}
	f->st = keep;
}

bool custom_ssd16xx_fast_pending(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;

	return !fast_rows_empty(&data->fast.act) || data->fast.hold_pending;
}

int custom_ssd16xx_fast_submit(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;
	struct custom_ssd16xx_fast *f = &data->fast;
	const uint32_t *nw = (const uint32_t *)data->bw_plane;
	uint32_t *tw = (uint32_t *)data->prev_bw_plane;
	uint32_t kfill[FAST_CNT_PLANES], wfill[FAST_CNT_PLANES];
	uint32_t c0 = k_cycle_get_32();
	struct fast_rows hit;

	fast_check_foreign(data);

	for (size_t i = 0; i < FAST_CNT_PLANES; i++) {
		kfill[i] = ((f->cfg.black_pulses >> i) & 1U) ? UINT32_MAX : 0U;
		wfill[i] = ((f->cfg.white_pulses >> i) & 1U) ? UINT32_MAX : 0U;
	}

	/* New target = framebuffer. Changed pixels (re)start their budget for
	 * the new direction; unchanged ones keep whatever they had left. */
	fast_rows_clear(&hit);
	for (uint16_t y = f->fb_dirty.y0; y <= f->fb_dirty.y1; y++) {
		size_t base = (size_t)y * FAST_WORDS_PER_ROW;
		bool row_hit = false;

		for (size_t w = base; w < base + FAST_WORDS_PER_ROW; w++) {
			uint32_t t = nw[w];
			uint32_t ch = t ^ tw[w];

			if (ch == 0U) {
				continue;
			}
			row_hit = true;
			tw[w] = t;
			for (size_t i = 0; i < FAST_CNT_PLANES; i++) {
				uint32_t want = (t & wfill[i]) | (~t & kfill[i]);

				f->cnt[i][w] = (f->cnt[i][w] & ~ch) | (ch & want);
			}
		}
		if (row_hit) {
			fast_rows_add(&hit, y);
			fast_mark(f->red_rows, y);
			fast_mark(f->bw_rows, y);
		}
	}
	fast_rows_clear(&f->fb_dirty);
	if (!fast_rows_empty(&hit)) {
		fast_rows_add(&f->act, hit.y0);
		fast_rows_add(&f->act, hit.y1);
	}

	memset(&f->st, 0, sizeof(f->st));
	f->st.y0 = hit.y0;
	f->st.y1 = hit.y1;
	f->st.plan_us = k_cyc_to_us_floor32(k_cycle_get_32() - c0);
	f->t_start = c0;
	f->first_pending = !fast_rows_empty(&hit);
	return 0;
}

int custom_ssd16xx_fast_step(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;
	struct custom_ssd16xx_fast *f = &data->fast;
	uint32_t us;
	int32_t err;

	fast_check_foreign(data);

	bool drive = !fast_rows_empty(&f->act);

	if (!drive && !f->hold_pending) {
		return 0;
	}

	err = fast_prepare(dev);
	if (err < 0) {
		goto fail;
	}
	err = fast_activate(dev, FAST_SEQ_SCAN, &us);
	if (err < 0) {
		goto fail;
	}
	f->st.scans++;
	f->st.wave_us += us;
	f->st.wave_max_us = MAX(f->st.wave_max_us, us);
	if (f->first_pending) {
		f->first_pending = false;
		f->st.first_us = k_cyc_to_us_floor32(k_cycle_get_32() - f->t_start);
	}

	uint32_t c0 = k_cycle_get_32();

	if (drive) {
		fast_decrement(data);
	}
	f->hold_pending = drive && fast_rows_empty(&f->act) && f->cfg.hold_ending;
	f->st.calc_us += k_cyc_to_us_floor32(k_cycle_get_32() - c0);
	f->st.total_us = k_cyc_to_us_floor32(k_cycle_get_32() - f->t_start);

	if (custom_ssd16xx_fast_pending(dev)) {
		return 1;
	}
	if (!f->cfg.keep_power) {
		err = fast_activate(dev, FAST_SEQ_POWER_OFF, &us);
		if (err < 0) {
			goto fail;
		}
		f->analog_on = false;
	}
	return 0;

fail:
	/* Controller RAM/LUT state unknown: full re-entry on the next scan;
	 * budgets were not spent, so the retry delivers the same pulse. */
	f->synced = false;
	return err;
}

int custom_ssd16xx_fast_power_off(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;
	struct custom_ssd16xx_fast *f = &data->fast;
	uint32_t us;
	int32_t err;

	fast_check_foreign(data);
	if (!fast_rows_empty(&f->act)) {
		return -EBUSY;
	}
	if (f->hold_pending) {
		err = custom_ssd16xx_fast_step(dev);
		if (err < 0) {
			return err;
		}
	}
	if (!f->analog_on) {
		return 0;
	}
	err = fast_activate(dev, FAST_SEQ_POWER_OFF, &us);
	if (err < 0) {
		return err;
	}
	f->analog_on = false;
	return 0;
}

/* Submit, then scan until idle or max_scans scans ran (0 = no limit). */
static int custom_ssd16xx_fast_run(const struct device *dev, uint8_t max_scans)
{
	struct custom_ssd16xx_data *data = dev->data;
	int err = custom_ssd16xx_fast_submit(dev);
	uint32_t n = 0;

	while (err >= 0 && custom_ssd16xx_fast_pending(dev) &&
	       (max_scans == 0U || n < max_scans)) {
		err = custom_ssd16xx_fast_step(dev);
		n++;
	}
	if (err < 0) {
		LOG_ERR("EPD fast drive failed: %d", err);
		return err;
	}
	data->partial_count++;
	return 0;
}

int custom_ssd16xx_refresh_fast_drive(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;

	/* Same preconditions as the fast partial (see refresh_partial). */
	if (data->color_mode != CUSTOM_SSD16XX_MONO || !data->prev_valid ||
	    data->in_gray) {
		return custom_ssd16xx_do_full(dev);
	}
	return custom_ssd16xx_fast_run(dev, 0);
}

int custom_ssd16xx_fast_test_scan(const struct device *dev, uint32_t *wave_us)
{
	struct custom_ssd16xx_data *data = dev->data;
	int32_t err;

	fast_check_foreign(data);
	if (custom_ssd16xx_fast_pending(dev)) {
		return -EBUSY;
	}
	err = fast_prepare(dev);
	if (err == 0) {
		err = fast_activate(dev, FAST_SEQ_SCAN, wave_us);
	}
	if (err < 0) {
		data->fast.synced = false;
	}
	return err;
}

int custom_ssd16xx_fast_enable(const struct device *dev, bool enable)
{
	struct custom_ssd16xx_data *data = dev->data;

	data->fast.enabled = enable;
	return 0;
}

bool custom_ssd16xx_fast_enabled(const struct device *dev)
{
	struct custom_ssd16xx_data *data = dev->data;

	return data->fast.enabled;
}

int custom_ssd16xx_fast_set_cfg(const struct device *dev,
				const struct custom_ssd16xx_fast_cfg *cfg)
{
	struct custom_ssd16xx_data *data = dev->data;

	if (cfg->black_pulses < 1U || cfg->black_pulses > 15U ||
	    cfg->white_pulses < 1U || cfg->white_pulses > 15U ||
	    cfg->frames < 1U || cfg->frames > 15U || cfg->fr > 15U) {
		return -EINVAL;
	}
	data->fast.cfg = *cfg;
	data->fast.synced = false; /* reload the LUT before the next scan */
	return 0;
}

void custom_ssd16xx_fast_get_cfg(const struct device *dev,
				 struct custom_ssd16xx_fast_cfg *cfg)
{
	struct custom_ssd16xx_data *data = dev->data;

	*cfg = data->fast.cfg;
}

void custom_ssd16xx_fast_get_stats(const struct device *dev,
				   struct custom_ssd16xx_fast_stats *st)
{
	struct custom_ssd16xx_data *data = dev->data;

	*st = data->fast.st;
}
#endif /* CONFIG_LLSS_EPD_FAST_DRIVE */

int custom_ssd16xx_set_mono_threshold(const struct device *dev, uint8_t thr)
{
	struct custom_ssd16xx_data *data = dev->data;

	data->mono_thr = thr;
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
		/* A MONO<->GRAY2 transition resets the panel waveform state:
		 * force the next refresh to be full by invalidating the
		 * previous frame. GRAY_MARK is a render-only overlay mode —
		 * switching through it must keep the BW baseline valid.
		 */
		bool gray2_crossing = (mode == CUSTOM_SSD16XX_GRAY2 ||
				       data->color_mode == CUSTOM_SSD16XX_GRAY2);

		data->color_mode = mode;
		if (gray2_crossing) {
			data->prev_valid = false;
		}
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
/*
 * Native 1bpp write: the incoming buffer is already the panel's plane format
 * (packed, MSB = leftmost pixel). X_ALIGNMENT_WIDTH guarantees full-width
 * regions, so `height` rows of COLS bytes are contiguous in both source and
 * destination — the whole conversion is one memcpy per plane.
 *
 * In MONO a pixel is either both plane bits set (white) or both clear (black),
 * so bw and red carry the same bytes. do_partial() drives BW + prev,
 * do_full() drives BW + RED, so both must be populated.
 */
static int custom_ssd16xx_write_mono(const struct device *dev, const uint16_t y,
				     const struct display_buffer_descriptor *desc,
				     const uint8_t *src)
{
	struct custom_ssd16xx_data *data = dev->data;
	size_t row_bytes = desc->width / 8u;
	size_t len = row_bytes * desc->height;
	size_t off = (size_t)y * CUSTOM_SSD16XX_COLS;

	if (row_bytes != CUSTOM_SSD16XX_COLS ||
	    off + len > CUSTOM_SSD16XX_BUF_BYTES) {
		LOG_ERR("mono write out of range: y=%u w=%u h=%u",
			y, desc->width, desc->height);
		return -EINVAL;
	}

	memcpy(data->bw_plane + off, src, len);
	memcpy(data->red_plane + off, src, len);
	return 0;
}

static int custom_ssd16xx_write(const struct device *dev, const uint16_t x,
				const uint16_t y,
				const struct display_buffer_descriptor *desc,
				const void *buf)
{
	struct custom_ssd16xx_data *data = dev->data;
	const uint8_t *src = buf;
	bool dither = data->dither;
	bool mono = (data->color_mode == CUSTOM_SSD16XX_MONO);
	uint32_t t_cyc0 = k_cycle_get_32();

	custom_ssd16xx_note_rows(data, y, desc->height);

	/* Native 1bpp — no conversion needed at all. */
	if (data->pixel_format == PIXEL_FORMAT_MONO01) {
		int rc = custom_ssd16xx_write_mono(dev, y, desc, src);

		g_write_cycles += k_cycle_get_32() - t_cyc0;
		return rc;
	}

	/* Accumulate a whole destination byte (8 pixels) in a register and touch
	 * each PSRAM plane once per byte, instead of a read-modify-write per
	 * pixel per plane. Measured 2026-07-23: ~85 ms vs ~107 ms for a full
	 * 800x480 frame. The per-pixel g2 computation is unchanged — only the
	 * store pattern differs. Row-base multiply is hoisted out of the loop. */
	for (uint16_t row = 0; row < desc->height; row++) {
		const uint8_t *srow = src + (size_t)row * desc->pitch;
		uint16_t py = y + row;
		size_t row_base = (size_t)py * CUSTOM_SSD16XX_COLS;
		uint16_t col = 0;

		while (col < desc->width) {
			uint16_t px = x + col;
			size_t byte_idx = row_base + px / 8u;
			uint8_t bit_pos = 7u - (px % 8u);
			/* Pixels remaining in this destination byte. */
			uint16_t n = MIN((uint16_t)(bit_pos + 1u),
					 (uint16_t)(desc->width - col));
			uint8_t mask = 0, bw_bits = 0, red_bits = 0;

			for (uint16_t i = 0; i < n; i++) {
				uint8_t luma = srow[col + i];
				uint16_t ppx = px + i;
				uint8_t g2;

				if (data->color_mode == CUSTOM_SSD16XX_GRAY_MARK) {
					/* Incremental-gray marking pass: mid-tones become
					 * the enhance LUT's codes (1 = light gray, 2 = dark
					 * gray), extremes become 0 = untouched. Bands assume
					 * the BW baseline was rendered with threshold 64, so
					 * every coded pixel is currently white on glass. */
					if (luma < 64u) {
						g2 = 0x0u;          /* black, leave */
					} else if (luma < 128u) {
						g2 = 0x2u;          /* dark gray */
					} else if (luma < 224u) {
						g2 = 0x1u;          /* light gray */
					} else {
						g2 = 0x0u;          /* white, leave */
					}
				} else if (mono) {
					/* 1bpp, partial-refresh capable. Arbitrary grays are
					 * ordered-dithered DOWN to pure B/W: the shade comes
					 * from the dot pattern, not a gray waveform, so
					 * dithered UI (e.g. the clock) stays partial-capable.
					 * Without dither, a hard threshold preserves an
					 * already-dithered frame's 0/255 instead of
					 * re-dithering it. */
					if (dither) {
						int32_t thr = bayer4[py & 3][ppx & 3] * 16 + 8; /* 8..248 */

						g2 = (luma > thr) ? 0x3u : 0x0u;
					} else {
						g2 = (luma >= data->mono_thr) ? 0x3u : 0x0u;
					}
				} else {
					/* GRAY2: real 4-level waveform, full refresh only. */
					g2 = quantise_2bpp(luma, dither, ppx, py);
				}

				/* Leftmost pixel of the byte is the MSB, so successive
				 * pixels walk the bit position downward. */
				uint8_t bit = BIT(bit_pos - i);

				mask |= bit;
				if (g2 & 0x01u) {
					bw_bits |= bit;
				}
				if (g2 & 0x02u) {
					red_bits |= bit;
				}
			}

			if (mask == 0xFFu) {
				/* Whole byte covered — no need to read PSRAM first. */
				data->bw_plane[byte_idx]  = bw_bits;
				data->red_plane[byte_idx] = red_bits;
			} else {
				data->bw_plane[byte_idx] =
					(uint8_t)((data->bw_plane[byte_idx] & ~mask) | bw_bits);
				data->red_plane[byte_idx] =
					(uint8_t)((data->red_plane[byte_idx] & ~mask) | red_bits);
			}

			col += n;
		}
	}

	g_write_cycles += k_cycle_get_32() - t_cyc0;
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
	/* Native 1bpp: the panel plane format IS LVGL's I1 draw-buffer format,
	 * so nothing has to be converted. MONO01 = bit set is white, matching
	 * the planes. X_ALIGNMENT_WIDTH makes every write full-width, so a
	 * region is a contiguous run of 100-byte rows -> write() is a memcpy.
	 *
	 * The L8 path below (ordered dithering, GRAY2, GRAY_MARK) is dormant in
	 * this configuration, not deleted: revive it by advertising
	 * PIXEL_FORMAT_L_8 here and setting CONFIG_LV_COLOR_DEPTH_8=y. */
	caps->supported_pixel_formats = PIXEL_FORMAT_MONO01 | PIXEL_FORMAT_L_8;
	caps->current_pixel_format = ((struct custom_ssd16xx_data *)dev->data)->pixel_format;
	caps->screen_info = SCREEN_INFO_EPD | SCREEN_INFO_MONO_MSB_FIRST |
			    SCREEN_INFO_X_ALIGNMENT_WIDTH;
}

static int custom_ssd16xx_set_pixel_format(const struct device *dev,
					   const enum display_pixel_format pf)
{
	struct custom_ssd16xx_data *data = dev->data;

	if (pf != PIXEL_FORMAT_MONO01 && pf != PIXEL_FORMAT_L_8) {
		return -ENOTSUP;
	}
	data->pixel_format = pf;
	return 0;
}

static int custom_ssd16xx_init(const struct device *dev)
{
	const struct custom_ssd16xx_config *config = dev->config;
	struct custom_ssd16xx_data *data = dev->data;

	/* Native 1bpp by default; see get_capabilities() for how to revive the
	 * dormant L8 gray/dither path. */
	data->pixel_format = PIXEL_FORMAT_MONO01;

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

#ifdef CONFIG_LLSS_EPD_FAST_DRIVE
	/* A submit only rewrites the budgets of changed pixels: everything
	 * else must start at zero, and SPIRAM .bss carries no such promise. */
	for (size_t i = 0; i < FAST_CNT_PLANES; i++) {
		memset(data->fast.cnt[i], 0, CUSTOM_SSD16XX_BUF_BYTES);
	}
#endif

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

	custom_ssd16xx_note_rows(data, 0, CUSTOM_SSD16XX_ROWS);
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
	data->updates++;
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
	custom_ssd16xx_note_rows(data, 0, CUSTOM_SSD16XX_ROWS);
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

int custom_ssd16xx_test_box(const struct device *dev, uint16_t x, uint16_t y,
			    uint16_t w, uint16_t h, uint8_t level)
{
	struct custom_ssd16xx_data *data = dev->data;
	uint16_t x1 = (uint16_t)MIN((uint32_t)x + w, 800U);
	uint16_t y1 = (uint16_t)MIN((uint32_t)y + h, CUSTOM_SSD16XX_ROWS);

	custom_ssd16xx_note_rows(data, y, h);
	for (uint16_t py = y; py < y1; py++) {
		for (uint16_t px = x; px < x1; px++) {
			uint8_t lv = level;

			if (lv != 0U && lv != 3U) {
				size_t byte_idx = (size_t)py * CUSTOM_SSD16XX_COLS + px / 8u;

				lv = (data->bw_plane[byte_idx] & BIT(7u - (px % 8u))) ? 0u : 3u;
			}
			test_set_px(data, px, py, lv);
		}
	}
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

#ifdef CONFIG_LLSS_EPD_FAST_DRIVE
#define CUSTOM_SSD16XX_FAST_BUFS(inst) \
	static uint32_t custom_ssd16xx_fcnt_##inst[FAST_CNT_PLANES][FAST_WORDS] \
		__attribute__((section(".ext_ram.bss"))); \
	static uint32_t custom_ssd16xx_fred_##inst[FAST_WORDS] \
		__attribute__((section(".ext_ram.bss")));
#define CUSTOM_SSD16XX_FAST_INIT(inst) \
	.fast = { \
		.cnt = { custom_ssd16xx_fcnt_##inst[0], custom_ssd16xx_fcnt_##inst[1], \
			 custom_ssd16xx_fcnt_##inst[2], custom_ssd16xx_fcnt_##inst[3] }, \
		.red = custom_ssd16xx_fred_##inst, \
		.cfg = CUSTOM_SSD16XX_FAST_CFG_DEFAULT, \
		.act = { .y0 = CUSTOM_SSD16XX_ROWS, .y1 = 0 }, \
		.fb_dirty = { .y0 = CUSTOM_SSD16XX_ROWS, .y1 = 0 }, \
	},
#else
#define CUSTOM_SSD16XX_FAST_BUFS(inst)
#define CUSTOM_SSD16XX_FAST_INIT(inst)
#endif

#define CUSTOM_SSD16XX_DEFINE(inst) \
	/* 48 KB each — placed in SPIRAM to keep DRAM free. Word-aligned: the \
	 * fast engine walks the planes 32 pixels at a time. */ \
	static uint8_t custom_ssd16xx_bw_##inst[CUSTOM_SSD16XX_BUF_BYTES] \
		__aligned(4) __attribute__((section(".ext_ram.bss"))); \
	static uint8_t custom_ssd16xx_red_##inst[CUSTOM_SSD16XX_BUF_BYTES] \
		__aligned(4) __attribute__((section(".ext_ram.bss"))); \
	static uint8_t custom_ssd16xx_prev_bw_##inst[CUSTOM_SSD16XX_BUF_BYTES] \
		__aligned(4) __attribute__((section(".ext_ram.bss"))); \
	CUSTOM_SSD16XX_FAST_BUFS(inst) \
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
		.mono_thr = 128, \
		.ram_window_full = true, \
		CUSTOM_SSD16XX_FAST_INIT(inst) \
	}; \
	DEVICE_DT_INST_DEFINE(inst, custom_ssd16xx_init, NULL, \
		&custom_ssd16xx_data_##inst, &custom_ssd16xx_cfg_##inst, \
		POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY, &custom_ssd16xx_api);

DT_INST_FOREACH_STATUS_OKAY(CUSTOM_SSD16XX_DEFINE)