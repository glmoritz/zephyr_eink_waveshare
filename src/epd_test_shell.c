/*
 * `epd` shell command group — waveform/refresh test harness for the SSD1677
 * panel (see docs/epd/WAVEFORMS.md). Lets us A/B update sequences, borders and
 * temperature banks over telnet without recompiling:
 *
 *   epd pattern digits          # fill framebuffer (not yet displayed)
 *   epd full | epd partial      # drive the normal refresh paths
 *   epd seq f7                  # raw 0x22 sequence, RAM planes rewritten
 *   epd seq d7 - 6a             # fast-full: temp bank 0x6A, border untouched
 *   epd seq f7 - - bw           # vendor Display(): BW RAM only
 *   epd status                  # counters + last sequence/duration
 *   epd floor 20                # auto full-refresh floor (0 = off)
 *   epd hold on|off             # freeze normal pipeline panel access
 *   epd gauntlet [n]            # baseline + n digits/white partial cycles
 *
 * Refreshes are serialized against the display pipeline via lvgl_mutex. A
 * server frame or device-UI repaint will overwrite the test pattern on its
 * next flush — quiesce (no moves, no device UI) while testing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#if DT_HAS_COMPAT_STATUS_OKAY(custom_ssd16xx_800x480)

#include "custom_ssd16xx.h"
#include "device_ui.h"
#include "display_thread.h"
#include "input_events.h"

static const struct device *const epd_dev =
	DEVICE_DT_GET_ANY(custom_ssd16xx_800x480);

static int parse_hex_or_skip(const char *arg, int16_t *out)
{
	char *end;
	long v;

	if (arg[0] == '-' && arg[1] == '\0') {
		*out = -1;
		return 0;
	}
	v = strtol(arg, &end, 16);
	if (*end != '\0' || v < 0 || v > 0xFF) {
		return -EINVAL;
	}
	*out = (int16_t)v;
	return 0;
}

static int cmd_epd_pattern(const struct shell *sh, size_t argc, char **argv)
{
	static const struct {
		const char *name;
		enum custom_ssd16xx_test_pattern pat;
	} pats[] = {
		{ "white",    CUSTOM_SSD16XX_PAT_WHITE },
		{ "black",    CUSTOM_SSD16XX_PAT_BLACK },
		{ "checker",  CUSTOM_SSD16XX_PAT_CHECKER },
		{ "digits",   CUSTOM_SSD16XX_PAT_DIGITS },
		{ "gradient", CUSTOM_SSD16XX_PAT_GRADIENT },
		{ "dither1",  CUSTOM_SSD16XX_PAT_DITHER1 },
		{ "dither2",  CUSTOM_SSD16XX_PAT_DITHER2 },
		{ "hlines",   CUSTOM_SSD16XX_PAT_HLINES },
		{ "vlines",   CUSTOM_SSD16XX_PAT_VLINES },
	};

	for (size_t i = 0; i < ARRAY_SIZE(pats); i++) {
		if (strcmp(argv[1], pats[i].name) == 0) {
			k_mutex_lock(&lvgl_mutex, K_FOREVER);
			int err = custom_ssd16xx_test_fill(epd_dev, pats[i].pat);

			k_mutex_unlock(&lvgl_mutex);
			if (err) {
				shell_error(sh, "fill failed: %d", err);
				return err;
			}
			shell_print(sh, "pattern '%s' in framebuffer; run "
				    "'epd full|partial|seq' to display", argv[1]);
			return 0;
		}
	}
	shell_error(sh, "unknown pattern '%s'", argv[1]);
	return -EINVAL;
}

static void print_status(const struct shell *sh)
{
	struct custom_ssd16xx_status st;

	custom_ssd16xx_get_status(epd_dev, &st);
	shell_print(sh,
		    "partials since full: %u  floor: %u  prev_valid: %d  "
		    "mode: %s  last: seq=0x%02X %u ms",
		    st.partial_count, st.full_refresh_interval, st.prev_valid,
		    st.color_mode == CUSTOM_SSD16XX_MONO ? "mono" : "gray2",
		    st.last_seq, st.last_refresh_ms);
}

static int cmd_epd_full(const struct shell *sh, size_t argc, char **argv)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	int err = custom_ssd16xx_refresh_full(epd_dev);

	k_mutex_unlock(&lvgl_mutex);
	if (err) {
		shell_error(sh, "full refresh failed: %d", err);
		return err;
	}
	print_status(sh);
	return 0;
}

static int cmd_epd_fast(const struct shell *sh, size_t argc, char **argv)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	int err = custom_ssd16xx_refresh_fast(epd_dev);

	k_mutex_unlock(&lvgl_mutex);
	if (err) {
		shell_error(sh, "fast-full refresh failed: %d", err);
		return err;
	}
	print_status(sh);
	return 0;
}

static int cmd_epd_partial(const struct shell *sh, size_t argc, char **argv)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	int err = custom_ssd16xx_refresh_partial(epd_dev);

	k_mutex_unlock(&lvgl_mutex);
	if (err) {
		shell_error(sh, "partial refresh failed: %d", err);
		return err;
	}
	print_status(sh);
	return 0;
}

static int cmd_epd_dpartial(const struct shell *sh, size_t argc, char **argv)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	int err = custom_ssd16xx_refresh_partial_deep(epd_dev);

	k_mutex_unlock(&lvgl_mutex);
	if (err) {
		shell_error(sh, "deep partial failed: %d", err);
		return err;
	}
	print_status(sh);
	return 0;
}

static int cmd_epd_gray(const struct shell *sh, size_t argc, char **argv)
{
	bool revert = strcmp(argv[0], "revert") == 0;

	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	int err = custom_ssd16xx_refresh_gray(epd_dev, revert);

	k_mutex_unlock(&lvgl_mutex);
	if (err) {
		shell_error(sh, "gray %s failed: %d", argv[0], err);
		return err;
	}
	print_status(sh);
	return 0;
}

static int cmd_epd_seq(const struct shell *sh, size_t argc, char **argv)
{
	int16_t seq, border = -1, temp = -1;
	enum custom_ssd16xx_test_ram ram = CUSTOM_SSD16XX_RAM_BOTH;

	if (parse_hex_or_skip(argv[1], &seq) < 0 || seq < 0) {
		shell_error(sh, "bad 0x22 value '%s'", argv[1]);
		return -EINVAL;
	}
	if (argc > 2 && parse_hex_or_skip(argv[2], &border) < 0) {
		shell_error(sh, "bad border '%s' (hex or -)", argv[2]);
		return -EINVAL;
	}
	if (argc > 3 && parse_hex_or_skip(argv[3], &temp) < 0) {
		shell_error(sh, "bad temp '%s' (hex or -)", argv[3]);
		return -EINVAL;
	}
	if (argc > 4) {
		if (strcmp(argv[4], "both") == 0) {
			ram = CUSTOM_SSD16XX_RAM_BOTH;
		} else if (strcmp(argv[4], "bw") == 0) {
			ram = CUSTOM_SSD16XX_RAM_BW;
		} else if (strcmp(argv[4], "none") == 0) {
			ram = CUSTOM_SSD16XX_RAM_NONE;
		} else {
			shell_error(sh, "ram must be both|bw|none");
			return -EINVAL;
		}
	}

	shell_print(sh, "seq=0x%02X border=%d temp=%d ram=%s ...",
		    (uint8_t)seq, border, temp, argc > 4 ? argv[4] : "both");

	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	int err = custom_ssd16xx_test_sequence(epd_dev, (uint8_t)seq, border,
					       temp, ram);

	k_mutex_unlock(&lvgl_mutex);
	if (err) {
		shell_error(sh, "sequence failed: %d", err);
		return err;
	}
	print_status(sh);
	return 0;
}

static int cmd_epd_status(const struct shell *sh, size_t argc, char **argv)
{
	print_status(sh);
	return 0;
}

static int cmd_epd_saver(const struct shell *sh, size_t argc, char **argv)
{
	device_ui_force_saver();
	shell_print(sh, "screensaver forced (any key returns to main)");
	return 0;
}

static int cmd_epd_cmp(const struct shell *sh, size_t argc, char **argv)
{
	struct custom_ssd16xx_plane_cmp c;

	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	custom_ssd16xx_test_compare(epd_dev, &c);
	k_mutex_unlock(&lvgl_mutex);

	shell_print(sh, "bw!=red: %u bytes (first at %u; row %u)  bw ink %u%%  "
		    "red ink %u%%  bw!=prev: %u bytes",
		    c.diff_bytes,
		    c.first_diff == UINT32_MAX ? 0 : c.first_diff,
		    c.first_diff == UINT32_MAX ? 0 : c.first_diff / 100u,
		    c.bw_ink_pct, c.red_ink_pct, c.prev_diff_bytes);
	return 0;
}

static int cmd_epd_reg(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t data[8];
	int16_t v;

	if (parse_hex_or_skip(argv[1], &v) < 0 || v < 0) {
		shell_error(sh, "bad command byte '%s'", argv[1]);
		return -EINVAL;
	}

	uint8_t cmd = (uint8_t)v;
	size_t len = argc - 2;

	if (len > sizeof(data)) {
		shell_error(sh, "max %u data bytes", (unsigned int)sizeof(data));
		return -EINVAL;
	}
	for (size_t i = 0; i < len; i++) {
		if (parse_hex_or_skip(argv[2 + i], &v) < 0 || v < 0) {
			shell_error(sh, "bad data byte '%s'", argv[2 + i]);
			return -EINVAL;
		}
		data[i] = (uint8_t)v;
	}

	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	int err = custom_ssd16xx_test_reg(epd_dev, cmd, data, len);

	k_mutex_unlock(&lvgl_mutex);
	if (err) {
		shell_error(sh, "reg write failed: %d", err);
		return err;
	}
	shell_print(sh, "reg 0x%02X written (%u data bytes)", cmd,
		    (unsigned int)len);
	return 0;
}

static int cmd_epd_snap(const struct shell *sh, size_t argc, char **argv)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	custom_ssd16xx_test_snap(epd_dev);
	k_mutex_unlock(&lvgl_mutex);
	shell_print(sh, "BW plane snapshotted");
	return 0;
}

static int cmd_epd_restore(const struct shell *sh, size_t argc, char **argv)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	int err = custom_ssd16xx_test_restore(epd_dev);

	k_mutex_unlock(&lvgl_mutex);
	if (err) {
		shell_error(sh, "no snapshot taken yet");
		return err;
	}
	shell_print(sh, "snapshot restored into both planes; run a sequence");
	return 0;
}

static int cmd_epd_dump(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	long row0 = strtol(argv[1], &end, 10);
	long nrows = 1;

	if (*end != '\0' || row0 < 0 || row0 > 479) {
		shell_error(sh, "row must be 0..479");
		return -EINVAL;
	}
	if (argc > 2) {
		nrows = strtol(argv[2], &end, 10);
		if (*end != '\0' || nrows < 1 || row0 + nrows > 480) {
			shell_error(sh, "bad row count");
			return -EINVAL;
		}
	}

	for (long r = row0; r < row0 + nrows; r++) {
		uint8_t buf[100];
		char hex[201];

		k_mutex_lock(&lvgl_mutex, K_FOREVER);
		custom_ssd16xx_test_row(epd_dev, (uint16_t)r, buf);
		k_mutex_unlock(&lvgl_mutex);

		for (int i = 0; i < 100; i++) {
			snprintf(&hex[i * 2], 3, "%02x", buf[i]);
		}
		shell_print(sh, "R%03ld:%s", r, hex);
	}
	return 0;
}

static int cmd_epd_hold(const struct shell *sh, size_t argc, char **argv)
{
	if (strcmp(argv[1], "on") == 0) {
		ui_test_hold(true);
		shell_print(sh, "hold ON: server frames / device UI won't touch the panel");
	} else if (strcmp(argv[1], "off") == 0) {
		ui_test_hold(false);
		shell_print(sh, "hold OFF: normal operation resumes on the next flush");
	} else {
		shell_error(sh, "usage: epd hold on|off");
		return -EINVAL;
	}
	return 0;
}

/*
 * Ghost gauntlet: factory-clean white baseline, then N cycles of
 * digits/white driven by partial refreshes only (floor suspended). Ends on
 * the ghost-inspection white screen; run a candidate full afterwards:
 *   epd seq f7          (factory full)
 *   epd seq d7 - 6a     (factory fast bank)
 *   epd full            (current custom-LUT full)
 */
static int cmd_epd_gauntlet(const struct shell *sh, size_t argc, char **argv)
{
	long cycles = 10;
	struct custom_ssd16xx_status st;
	int err;

	if (argc > 1) {
		char *end;

		cycles = strtol(argv[1], &end, 10);
		if (*end != '\0' || cycles < 1 || cycles > 100) {
			shell_error(sh, "cycles must be 1..100");
			return -EINVAL;
		}
	}

	if (!ui_test_hold_active()) {
		ui_test_hold(true);
		shell_print(sh, "hold ON (remember 'epd hold off' when done)");
	}

	custom_ssd16xx_get_status(epd_dev, &st);
	custom_ssd16xx_set_full_refresh_interval(epd_dev, 0);

	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	err = custom_ssd16xx_test_fill(epd_dev, CUSTOM_SSD16XX_PAT_WHITE);
	if (err == 0) {
		err = custom_ssd16xx_test_sequence(epd_dev, 0xF7, -1, -1,
						   CUSTOM_SSD16XX_RAM_BOTH);
	}
	k_mutex_unlock(&lvgl_mutex);
	if (err) {
		goto out;
	}
	shell_print(sh, "baseline: white via 0xF7");

	for (long i = 1; i <= cycles; i++) {
		k_mutex_lock(&lvgl_mutex, K_FOREVER);
		err = custom_ssd16xx_test_fill(epd_dev, CUSTOM_SSD16XX_PAT_DIGITS);
		if (err == 0) {
			err = custom_ssd16xx_refresh_partial(epd_dev);
		}
		if (err == 0) {
			err = custom_ssd16xx_test_fill(epd_dev, CUSTOM_SSD16XX_PAT_WHITE);
		}
		if (err == 0) {
			err = custom_ssd16xx_refresh_partial(epd_dev);
		}
		k_mutex_unlock(&lvgl_mutex);
		if (err) {
			goto out;
		}
		shell_print(sh, "cycle %ld/%ld done", i, cycles);
	}

out:
	custom_ssd16xx_set_full_refresh_interval(epd_dev, st.full_refresh_interval);
	if (err) {
		shell_error(sh, "gauntlet aborted: %d", err);
		return err;
	}
	shell_print(sh, "gauntlet done (%ld cycles = %ld partials). Inspect the "
		    "white screen for digit stains, then run a candidate full "
		    "(seq f7 | seq d7 - 6a | full).", cycles, cycles * 2);
	print_status(sh);
	return 0;
}

static int parse_long(const struct shell *sh, const char *arg, long lo,
		      long hi, long *out)
{
	char *end;
	long v = strtol(arg, &end, 0);

	if (*end != '\0' || v < lo || v > hi) {
		shell_error(sh, "'%s' out of range %ld..%ld", arg, lo, hi);
		return -EINVAL;
	}
	*out = v;
	return 0;
}

static int cmd_epd_box(const struct shell *sh, size_t argc, char **argv)
{
	long v[4];
	uint8_t level = 1; /* invert */

	for (int i = 0; i < 4; i++) {
		if (parse_long(sh, argv[1 + i], 0, i % 2 ? 480 : 800, &v[i])) {
			return -EINVAL;
		}
	}
	if (argc > 5) {
		if (strcmp(argv[5], "black") == 0) {
			level = 0;
		} else if (strcmp(argv[5], "white") == 0) {
			level = 3;
		} else if (strcmp(argv[5], "invert") != 0) {
			shell_error(sh, "fill must be black|white|invert");
			return -EINVAL;
		}
	}

	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	custom_ssd16xx_test_box(epd_dev, v[0], v[1], v[2], v[3], level);
	k_mutex_unlock(&lvgl_mutex);
	shell_print(sh, "box %ldx%ld at (%ld,%ld) in framebuffer", v[2], v[3],
		    v[0], v[1]);
	return 0;
}

/* Cosmetic press feedback only (the strip invert a key-down shows): nothing
 * is sent to the server, so it is safe to time against a live session. */
static int cmd_epd_press(const struct shell *sh, size_t argc, char **argv)
{
	if (strcmp(argv[1], "clear") == 0) {
		ui_press_feedback_clear();
		shell_print(sh, "press feedback cleared");
		return 0;
	}

	enum ui_btn btn = ui_btn_from_name(argv[1]);

	if (btn == UI_BTN_NONE) {
		shell_error(sh, "unknown button '%s' (1..8, ENTER, ESC, ...)", argv[1]);
		return -EINVAL;
	}
	ui_press_feedback_request(btn);
	shell_print(sh, "press feedback for %s requested (local only)", argv[1]);
	return 0;
}

#ifdef CONFIG_LLSS_EPD_FAST_DRIVE
#define US_MS(us) (us) / 1000U, ((us) % 1000U) / 10U

static void print_fast_stats(const struct shell *sh, const char *label)
{
	struct custom_ssd16xx_fast_stats st;

	custom_ssd16xx_fast_get_stats(epd_dev, &st);
	shell_print(sh, "%s: scans=%u first=%u.%02u ms total=%u.%02u ms rows %u..%u",
		    label, st.scans, US_MS(st.first_us), US_MS(st.total_us),
		    st.y0, st.y1);
	shell_print(sh, "  plan=%u sync=%u power=%u calc=%u spi=%u (%u rows) "
		    "wave=%u (max %u/scan) us%s",
		    st.plan_us, st.sync_us, st.power_us, st.calc_us, st.spi_us,
		    st.rows, st.wave_us, st.wave_max_us,
		    st.busy_miss ? "  BUSY-MISS!" : "");
}

static void print_fast_cfg(const struct shell *sh)
{
	struct custom_ssd16xx_fast_cfg c;

	custom_ssd16xx_fast_get_cfg(epd_dev, &c);
	shell_print(sh, "fast cfg: k=%u w=%u n=%u fr=%u vss=%d hold=%d rskip=%d "
		    "pwr=%d s=%u  fmode=%s", c.black_pulses, c.white_pulses,
		    c.frames, c.fr, c.vss_frame, c.hold_ending, c.red_skip,
		    c.keep_power, c.sync_scans,
		    custom_ssd16xx_fast_enabled(epd_dev) ? "on" : "off");
}

/* epd fcfg [k=N] [w=N] [n=N] [fr=N] [vss=0|1] [hold=0|1] [rskip=0|1] [pwr=0|1]
 *           [s=N]  (s = scans refresh_partial runs before returning, 0 = all) */
static int cmd_epd_fcfg(const struct shell *sh, size_t argc, char **argv)
{
	struct custom_ssd16xx_fast_cfg c;

	custom_ssd16xx_fast_get_cfg(epd_dev, &c);
	for (size_t i = 1; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		long v;

		if (!eq || parse_long(sh, eq + 1, 0, 15, &v)) {
			shell_error(sh, "expected key=value, got '%s'", argv[i]);
			return -EINVAL;
		}
		*eq = '\0';
		if (strcmp(argv[i], "k") == 0) {
			c.black_pulses = v;
		} else if (strcmp(argv[i], "w") == 0) {
			c.white_pulses = v;
		} else if (strcmp(argv[i], "n") == 0) {
			c.frames = v;
		} else if (strcmp(argv[i], "fr") == 0) {
			c.fr = v;
		} else if (strcmp(argv[i], "vss") == 0) {
			c.vss_frame = v != 0;
		} else if (strcmp(argv[i], "hold") == 0) {
			c.hold_ending = v != 0;
		} else if (strcmp(argv[i], "rskip") == 0) {
			c.red_skip = v != 0;
		} else if (strcmp(argv[i], "pwr") == 0) {
			c.keep_power = v != 0;
		} else if (strcmp(argv[i], "s") == 0) {
			c.sync_scans = v;
		} else {
			shell_error(sh, "unknown key '%s'", argv[i]);
			return -EINVAL;
		}
	}
	if (argc > 1) {
		k_mutex_lock(&lvgl_mutex, K_FOREVER);
		int err = custom_ssd16xx_fast_set_cfg(epd_dev, &c);

		k_mutex_unlock(&lvgl_mutex);
		if (err) {
			shell_error(sh, "rejected (pulses/n 1..15, fr 0..15)");
			return err;
		}
	}
	print_fast_cfg(sh);
	return 0;
}

static int cmd_epd_fstat(const struct shell *sh, size_t argc, char **argv)
{
	print_fast_cfg(sh);
	print_fast_stats(sh, custom_ssd16xx_fast_pending(epd_dev) ?
			 "last submit (still driving)" : "last submit");
	return 0;
}

static int cmd_epd_fdrive(const struct shell *sh, size_t argc, char **argv)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	int err = custom_ssd16xx_refresh_fast_drive(epd_dev);

	k_mutex_unlock(&lvgl_mutex);
	if (err) {
		shell_error(sh, "fast drive failed: %d", err);
		return err;
	}
	print_fast_stats(sh, "fast");
	return 0;
}

/* Time bare scans: with the engine idle every pixel is on a HOLD row, so
 * nothing on glass moves — pure frame + activation overhead. */
static int fast_time_scans(const struct shell *sh, long count, uint32_t *avg)
{
	uint32_t sum = 0, mn = UINT32_MAX, mx = 0;

	for (long i = 0; i < count; i++) {
		uint32_t us;

		k_mutex_lock(&lvgl_mutex, K_FOREVER);
		int err = custom_ssd16xx_fast_test_scan(epd_dev, &us);

		k_mutex_unlock(&lvgl_mutex);
		if (err) {
			shell_error(sh, "scan failed: %d", err);
			return err;
		}
		/* The first scan of a session carries the RAM resync. */
		if (i == 0 && count > 1) {
			continue;
		}
		sum += us;
		mn = MIN(mn, us);
		mx = MAX(mx, us);
	}
	*avg = sum / (count > 1 ? count - 1 : 1);
	shell_print(sh, "  scan wave: min %u avg %u max %u us", mn, *avg, mx);
	return 0;
}

static int cmd_epd_fscan(const struct shell *sh, size_t argc, char **argv)
{
	long count = 6;
	uint32_t avg;

	if (argc > 1 && parse_long(sh, argv[1], 1, 100, &count)) {
		return -EINVAL;
	}
	print_fast_cfg(sh);
	return fast_time_scans(sh, count, &avg);
}

/* Frame time per LUT frame-rate nibble, measured on all-HOLD scans. */
static int cmd_epd_fsweep(const struct shell *sh, size_t argc, char **argv)
{
	static const uint8_t frs[] = { 0x0, 0x1, 0x2, 0x3, 0x4, 0x6, 0x8, 0xC, 0xF };
	struct custom_ssd16xx_fast_cfg c0, c;
	bool was_hold = ui_test_hold_active();
	int err = 0;

	ui_test_hold(true);
	custom_ssd16xx_fast_get_cfg(epd_dev, &c0);
	for (size_t i = 0; i < ARRAY_SIZE(frs) && err == 0; i++) {
		for (uint8_t n = 1; n <= 2 && err == 0; n++) {
			uint32_t avg;

			c = c0;
			c.fr = frs[i];
			c.frames = n;
			c.vss_frame = false;
			custom_ssd16xx_fast_set_cfg(epd_dev, &c);
			shell_print(sh, "fr=0x%X frames=%u:", frs[i], n);
			err = fast_time_scans(sh, 4, &avg);
		}
	}
	custom_ssd16xx_fast_set_cfg(epd_dev, &c0);
	ui_test_hold(was_hold);
	return err;
}

static int cmd_epd_fpoff(const struct shell *sh, size_t argc, char **argv)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	int err = custom_ssd16xx_fast_power_off(epd_dev);

	k_mutex_unlock(&lvgl_mutex);
	if (err) {
		shell_error(sh, "power off failed: %d", err);
		return err;
	}
	shell_print(sh, "analog rails off");
	return 0;
}

static int cmd_epd_fmode(const struct shell *sh, size_t argc, char **argv)
{
	if (strcmp(argv[1], "on") == 0) {
		custom_ssd16xx_fast_enable(epd_dev, true);
	} else if (strcmp(argv[1], "off") == 0) {
		k_mutex_lock(&lvgl_mutex, K_FOREVER);
		custom_ssd16xx_fast_enable(epd_dev, false);
		custom_ssd16xx_fast_power_off(epd_dev);
		k_mutex_unlock(&lvgl_mutex);
	} else {
		shell_error(sh, "usage: epd fmode on|off");
		return -EINVAL;
	}
	print_fast_cfg(sh);
	return 0;
}

struct bench_acc {
	uint32_t n, mn, mx;
	uint64_t sum;
};

static void acc_add(struct bench_acc *a, uint32_t us)
{
	a->mn = a->n ? MIN(a->mn, us) : us;
	a->mx = a->n ? MAX(a->mx, us) : us;
	a->sum += us;
	a->n++;
}

static void acc_print(const struct shell *sh, const char *label,
		      const struct bench_acc *a)
{
	uint32_t avg = a->n ? (uint32_t)(a->sum / a->n) : 0;

	shell_print(sh, "  %-34s n=%2u  min %4u.%02u  avg %4u.%02u  max %4u.%02u ms",
		    label, a->n, US_MS(a->mn), US_MS(avg), US_MS(a->mx));
}

static int bench_fill_full(enum custom_ssd16xx_test_pattern pat)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	int err = custom_ssd16xx_test_fill(epd_dev, pat);

	if (err == 0) {
		err = custom_ssd16xx_refresh_full(epd_dev);
	}
	k_mutex_unlock(&lvgl_mutex);
	return err;
}

static void bench_box(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	custom_ssd16xx_test_box(epd_dev, x, y, w, h, 1);
	k_mutex_unlock(&lvgl_mutex);
}

static void bench_fill(enum custom_ssd16xx_test_pattern pat)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	custom_ssd16xx_test_fill(epd_dev, pat);
	k_mutex_unlock(&lvgl_mutex);
}

/* One fast drive of the framebuffer; first/total/per-scan into the accs. */
static int bench_fast(struct bench_acc *first, struct bench_acc *total,
		      struct bench_acc *scan,
		      struct custom_ssd16xx_fast_stats *out)
{
	struct custom_ssd16xx_fast_stats st;

	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	int err = custom_ssd16xx_refresh_fast_drive(epd_dev);

	custom_ssd16xx_fast_get_stats(epd_dev, &st);
	k_mutex_unlock(&lvgl_mutex);
	if (err) {
		return err;
	}
	if (first) {
		acc_add(first, st.first_us);
	}
	if (total) {
		acc_add(total, st.total_us);
	}
	if (scan && st.scans) {
		acc_add(scan, st.wave_us / st.scans);
	}
	if (out) {
		*out = st;
	}
	return 0;
}

/* One OTP partial (pre + waveform + post), timed end to end. */
static int bench_partial(struct bench_acc *total)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);
	uint32_t c0 = k_cycle_get_32();
	int err = custom_ssd16xx_refresh_partial(epd_dev);
	uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - c0);

	k_mutex_unlock(&lvgl_mutex);
	if (err == 0) {
		acc_add(total, us);
	}
	return err;
}

/*
 * A/B latency bench: inkboy-style fast drive vs the current OTP partial,
 * on a press-feedback-sized box (invert, then invert back) and on a full
 * screen change (white <-> checker). Runs at elevated priority so the
 * numbers are the panel's, not the scheduler's. Ends on a clean full.
 */
static int cmd_epd_fbench(const struct shell *sh, size_t argc, char **argv)
{
	long iters = 5, bw = 200, bh = 64;
	struct bench_acc f_first = {0}, f_total = {0}, f_scan = {0};
	struct bench_acc s_first = {0}, s_total = {0}, p_box = {0}, p_scr = {0};
	struct custom_ssd16xx_fast_stats cold = {0}, pwr = {0};
	struct custom_ssd16xx_status st0;
	struct custom_ssd16xx_fast_cfg cfg;
	bool was_hold = ui_test_hold_active();
	bool was_fmode = custom_ssd16xx_fast_enabled(epd_dev);
	int prio = k_thread_priority_get(k_current_get());
	int err;

	if ((argc > 1 && parse_long(sh, argv[1], 1, 50, &iters)) ||
	    (argc > 2 && parse_long(sh, argv[2], 8, 800, &bw)) ||
	    (argc > 3 && parse_long(sh, argv[3], 8, 480, &bh))) {
		return -EINVAL;
	}

	uint16_t bx = 40, by = 480 - 40 - bh;

	ui_test_hold(true);
	custom_ssd16xx_get_status(epd_dev, &st0);
	custom_ssd16xx_set_full_refresh_interval(epd_dev, 0);
	custom_ssd16xx_fast_enable(epd_dev, false);
	k_thread_priority_set(k_current_get(), K_PRIO_PREEMPT(5));
	print_fast_cfg(sh);
	shell_print(sh, "fbench: %ld iters, box %ldx%ld at (%u,%u)", iters, bw,
		    bh, bx, by);

	/* Fast drive, box. The first drive carries session entry. */
	err = bench_fill_full(CUSTOM_SSD16XX_PAT_WHITE);
	bench_box(bx, by, bw, bh);
	err = err ? err : bench_fast(NULL, NULL, NULL, &cold);
	bench_box(bx, by, bw, bh);
	err = err ? err : bench_fast(NULL, NULL, NULL, NULL);
	for (long i = 0; i < iters && err == 0; i++) {
		bench_box(bx, by, bw, bh);
		err = bench_fast(&f_first, &f_total, &f_scan, NULL);
		bench_box(bx, by, bw, bh);
		err = err ? err : bench_fast(&f_first, &f_total, &f_scan, NULL);
	}
	shell_print(sh, "fast box done");

	/* Rails dropped: what a press after an idle power-down costs. */
	if (err == 0) {
		k_mutex_lock(&lvgl_mutex, K_FOREVER);
		err = custom_ssd16xx_fast_power_off(epd_dev);
		k_mutex_unlock(&lvgl_mutex);
		bench_box(bx, by, bw, bh);
		err = err ? err : bench_fast(NULL, NULL, NULL, &pwr);
		bench_box(bx, by, bw, bh);
		err = err ? err : bench_fast(NULL, NULL, NULL, NULL);
	}

	/* Fast drive, whole screen. */
	for (int i = 0; i < 2 && err == 0; i++) {
		bench_fill(CUSTOM_SSD16XX_PAT_CHECKER);
		err = bench_fast(&s_first, &s_total, NULL, NULL);
		bench_fill(CUSTOM_SSD16XX_PAT_WHITE);
		err = err ? err : bench_fast(&s_first, &s_total, NULL, NULL);
	}
	shell_print(sh, "fast full-screen done");

	/* OTP partial (today's path), same changes from a clean baseline. */
	err = err ? err : bench_fill_full(CUSTOM_SSD16XX_PAT_WHITE);
	for (long i = 0; i < iters && err == 0; i++) {
		bench_box(bx, by, bw, bh);
		err = bench_partial(&p_box);
		bench_box(bx, by, bw, bh);
		err = err ? err : bench_partial(&p_box);
	}
	for (int i = 0; i < 2 && err == 0; i++) {
		bench_fill(CUSTOM_SSD16XX_PAT_CHECKER);
		err = bench_partial(&p_scr);
		bench_fill(CUSTOM_SSD16XX_PAT_WHITE);
		err = err ? err : bench_partial(&p_scr);
	}
	shell_print(sh, "OTP partial done");

	/* Leave a clean panel behind. */
	err = err ? err : bench_fill_full(CUSTOM_SSD16XX_PAT_WHITE);

	k_thread_priority_set(k_current_get(), prio);
	custom_ssd16xx_set_full_refresh_interval(epd_dev, st0.full_refresh_interval);
	custom_ssd16xx_fast_enable(epd_dev, was_fmode);
	ui_test_hold(was_hold);
	if (err) {
		shell_error(sh, "fbench aborted: %d", err);
		return err;
	}

	custom_ssd16xx_fast_get_cfg(epd_dev, &cfg);
	shell_print(sh, "== fbench results (k=%u w=%u n=%u fr=%u) ==",
		    cfg.black_pulses, cfg.white_pulses, cfg.frames, cfg.fr);
	acc_print(sh, "fast box: first scan done", &f_first);
	acc_print(sh, "fast box: all pulses + HOLD", &f_total);
	acc_print(sh, "fast box: wave per scan", &f_scan);
	shell_print(sh, "  fast box, cold session: first %u.%02u ms (sync %u, "
		    "power-up %u, spi %u us)", US_MS(cold.first_us),
		    cold.sync_us, cold.power_us, cold.spi_us);
	shell_print(sh, "  fast box, rails were off: first %u.%02u ms (power-up %u us)",
		    US_MS(pwr.first_us), pwr.power_us);
	acc_print(sh, "fast full screen: first scan done", &s_first);
	acc_print(sh, "fast full screen: all pulses + HOLD", &s_total);
	acc_print(sh, "OTP partial box (complete)", &p_box);
	acc_print(sh, "OTP partial full screen (complete)", &p_scr);
	return 0;
}
#endif /* CONFIG_LLSS_EPD_FAST_DRIVE */

static int cmd_epd_floor(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	long v = strtol(argv[1], &end, 10);

	if (*end != '\0' || v < 0) {
		shell_error(sh, "bad floor '%s'", argv[1]);
		return -EINVAL;
	}
	custom_ssd16xx_set_full_refresh_interval(epd_dev, (uint32_t)v);
	shell_print(sh, "auto full-refresh floor = %ld", v);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_epd,
	SHELL_CMD_ARG(pattern, NULL,
		      "Fill framebuffer: white|black|checker|digits|gradient",
		      cmd_epd_pattern, 2, 0),
	SHELL_CMD_ARG(full, NULL, "Normal full refresh path", cmd_epd_full, 1, 0),
	SHELL_CMD_ARG(fast, NULL, "Fast-full refresh (hot OTP bank + 0xD7)",
		      cmd_epd_fast, 1, 0),
	SHELL_CMD_ARG(partial, NULL, "Normal partial refresh path",
		      cmd_epd_partial, 1, 0),
	SHELL_CMD_ARG(dpartial, NULL, "Retention-grade deep partial",
		      cmd_epd_dpartial, 1, 0),
	SHELL_CMD_ARG(enhance, NULL, "Gray pass over displayed BW page",
		      cmd_epd_gray, 1, 0),
	SHELL_CMD_ARG(revert, NULL, "Undo gray pass (back to BW)",
		      cmd_epd_gray, 1, 0),
	SHELL_CMD_ARG(seq, NULL,
		      "Raw update: <hex22> [border|-] [temp|-] [both|bw|none]",
		      cmd_epd_seq, 2, 3),
	SHELL_CMD_ARG(status, NULL, "Refresh counters and last duration",
		      cmd_epd_status, 1, 0),
	SHELL_CMD_ARG(cmp, NULL, "Diff BW vs RED planes + ink coverage",
		      cmd_epd_cmp, 1, 0),
	SHELL_CMD_ARG(reg, NULL, "Raw register write: <cmd_hex> [data_hex ...]",
		      cmd_epd_reg, 2, 8),
	SHELL_CMD_ARG(snap, NULL, "Snapshot current BW plane", cmd_epd_snap, 1, 0),
	SHELL_CMD_ARG(restore, NULL, "Restore snapshot into both planes",
		      cmd_epd_restore, 1, 0),
	SHELL_CMD_ARG(dump, NULL, "Hex-dump BW plane rows: <row> [nrows]",
		      cmd_epd_dump, 2, 1),
	SHELL_CMD_ARG(hold, NULL, "Freeze normal pipeline panel access: on|off",
		      cmd_epd_hold, 2, 0),
	SHELL_CMD_ARG(saver, NULL, "Force the screensaver on (test hook)",
		      cmd_epd_saver, 1, 0),
	SHELL_CMD_ARG(gauntlet, NULL,
		      "Ghost gauntlet: baseline + N digits/white partial cycles (default 10)",
		      cmd_epd_gauntlet, 1, 1),
	SHELL_CMD_ARG(floor, NULL, "Set auto full-refresh floor (0 = off)",
		      cmd_epd_floor, 2, 0),
	SHELL_CMD_ARG(press, NULL,
		      "Local press-feedback flash, nothing sent: <btn>|clear",
		      cmd_epd_press, 2, 0),
	SHELL_CMD_ARG(box, NULL,
		      "Paint a rect: <x> <y> <w> <h> [black|white|invert]",
		      cmd_epd_box, 5, 1),
#ifdef CONFIG_LLSS_EPD_FAST_DRIVE
	SHELL_CMD_ARG(fcfg, NULL,
		      "Fast-drive cfg: [k=] [w=] [n=] [fr=] [vss=] [hold=] [rskip=] [pwr=] [s=]",
		      cmd_epd_fcfg, 1, 9),
	SHELL_CMD_ARG(fdrive, NULL, "Fast-drive the framebuffer (inkboy engine)",
		      cmd_epd_fdrive, 1, 0),
	SHELL_CMD_ARG(fstat, NULL, "Fast-drive cfg + stats of the last submit",
		      cmd_epd_fstat, 1, 0),
	SHELL_CMD_ARG(fscan, NULL, "Time bare all-HOLD scans: [count]",
		      cmd_epd_fscan, 1, 1),
	SHELL_CMD_ARG(fsweep, NULL, "Scan time per LUT frame-rate nibble",
		      cmd_epd_fsweep, 1, 0),
	SHELL_CMD_ARG(fpoff, NULL, "Drop the analog rails", cmd_epd_fpoff, 1, 0),
	SHELL_CMD_ARG(fmode, NULL, "Route partial refreshes through fast drive: on|off",
		      cmd_epd_fmode, 2, 0),
	SHELL_CMD_ARG(fbench, NULL,
		      "Fast drive vs OTP partial latency: [iters] [box_w] [box_h]",
		      cmd_epd_fbench, 1, 3),
#endif
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(epd, &sub_epd, "SSD1677 waveform test harness", NULL);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(custom_ssd16xx_800x480) */
