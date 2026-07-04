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

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#if DT_HAS_COMPAT_STATUS_OKAY(custom_ssd16xx_800x480)

#include "custom_ssd16xx.h"
#include "display_thread.h"

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
	SHELL_CMD_ARG(gauntlet, NULL,
		      "Ghost gauntlet: baseline + N digits/white partial cycles (default 10)",
		      cmd_epd_gauntlet, 1, 1),
	SHELL_CMD_ARG(floor, NULL, "Set auto full-refresh floor (0 = off)",
		      cmd_epd_floor, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(epd, &sub_epd, "SSD1677 waveform test harness", NULL);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(custom_ssd16xx_800x480) */
