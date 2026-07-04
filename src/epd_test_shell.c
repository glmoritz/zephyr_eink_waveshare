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
 *
 * Refreshes are serialized against the display pipeline via lvgl_mutex. A
 * server frame or device-UI repaint will overwrite the test pattern on its
 * next flush — quiesce (no moves, no device UI) while testing.
 */

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
	SHELL_CMD_ARG(partial, NULL, "Normal partial refresh path",
		      cmd_epd_partial, 1, 0),
	SHELL_CMD_ARG(seq, NULL,
		      "Raw update: <hex22> [border|-] [temp|-] [both|bw|none]",
		      cmd_epd_seq, 2, 3),
	SHELL_CMD_ARG(status, NULL, "Refresh counters and last duration",
		      cmd_epd_status, 1, 0),
	SHELL_CMD_ARG(floor, NULL, "Set auto full-refresh floor (0 = off)",
		      cmd_epd_floor, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(epd, &sub_epd, "SSD1677 waveform test harness", NULL);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(custom_ssd16xx_800x480) */
