/*
 * Device-local UI — boot log console, network status, clock, and alarm.
 *
 * Three LVGL screen objects (log, network, clock) are created at init and
 * swapped in/out with lv_screen_load().  A dedicated low-priority thread
 * renders on signal (button press) or every 30 s (clock tick + alarm check).
 *
 * Button routing contract:
 *   MAIN screen   → ENTER LONG_PRESS opens device menu; all else → LLSS
 *   Device screens → HL_LEFT/RIGHT cycles screens; ESC exits; screen-specific
 *                    BTN_* are direct context actions (no cursor navigation)
 *
 * Screen cycle: LOG ←HL_LEFT/RIGHT→ NETWORK ←HL_LEFT/RIGHT→ CLOCK
 *               ←HL_LEFT/RIGHT→ ALARM → (wrap)
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/clock.h>

#include <lvgl.h>

LV_FONT_DECLARE(dseg_bold_italic_200);

#include "device_ui.h"
#include "display_thread.h"
#include "system_flags.h"
#include "wifi_prov.h"

LOG_MODULE_REGISTER(device_ui, LOG_LEVEL_INF);

/* =========================================================================
 * Log ring buffer
 * ========================================================================= */

/* 16 lines of montserrat_14 fit the content band (480 − header − footer)
 * without clipping the newest entries at the bottom. */
#define LOG_ENTRIES  16
#define LOG_MSG_LEN  64
#define LOG_TS_LEN   11  /* "HH:MM:SS" (8) or "T+XXXXXs" (9) + padding → 11 */

struct log_entry {
	int64_t uptime_s;
	int64_t realtime_s;  /* 0 = not yet synced */
	char    msg[LOG_MSG_LEN];
};

static struct log_entry log_buf[LOG_ENTRIES];
static int log_next;
static int log_count;
static K_MUTEX_DEFINE(log_mutex);

/* Pre-allocated render buffer — avoids stack pressure in render thread. */
#define LOG_TEXT_MAX (LOG_ENTRIES * (LOG_TS_LEN + 3 + LOG_MSG_LEN + 1))
static char log_text_buf[LOG_TEXT_MAX + 1];

/* =========================================================================
 * Alarm / timezone state
 * ========================================================================= */

static int  alarm_hour;
static int  alarm_min;
static bool alarm_enabled;
static bool clock_24h = true;
static int  tz_offset;        /* display time = UTC + tz_offset hours */
static bool alarm_fired;      /* latched within the trigger minute */
static bool settings_ready;
static K_MUTEX_DEFINE(ui_state_mutex);

/* =========================================================================
 * Screen selection
 * ========================================================================= */

enum dev_screen {
	SCR_MAIN = 0,
	SCR_LOG,
	SCR_NETWORK,
	SCR_CLOCK,
	SCR_ALARM,
};

static atomic_t active_scr = ATOMIC_INIT(SCR_CLOCK);

/* =========================================================================
 * LVGL objects
 * ========================================================================= */

static lv_obj_t *main_scr_ref;

/* Log screen */
static lv_obj_t *log_scr;
static lv_obj_t *log_content;

/* Network screen */
static lv_obj_t *net_scr;
static lv_obj_t *net_ssid_lbl;
static lv_obj_t *net_ip_lbl;

/* Clock screen */
static lv_obj_t *clk_scr;
static lv_obj_t *clk_date_lbl;
static lv_obj_t *clk_time_bg_lbl;
static lv_obj_t *clk_time_lbl;
static lv_obj_t *clk_status_lbl;
static lv_obj_t *clk_power_lbl;

/* Alarm screen */
static lv_obj_t *alm_scr;
static lv_obj_t *alm_date_lbl;
static lv_obj_t *alm_time_lbl;
static lv_obj_t *alm_meta_lbl;
static lv_obj_t *alm_alarm_lbl;

#define CLK_GHOST_TEXT "88:88"

/* =========================================================================
 * Wake signal
 * ========================================================================= */

static struct k_poll_signal ui_signal = K_POLL_SIGNAL_INITIALIZER(ui_signal);

static void signal_render(void)
{
	k_poll_signal_raise(&ui_signal, 0);
}

/* =========================================================================
 * Settings — alarm persistence
 * ========================================================================= */

static int alarm_settings_set(const char *key, size_t len,
			       settings_read_cb read_cb, void *cb_arg)
{
	int32_t val;

	if (len != sizeof(val)) {
		return -EINVAL;
	}
	read_cb(cb_arg, &val, sizeof(val));

	if (!strcmp(key, "hour")) {
		alarm_hour = val;
	} else if (!strcmp(key, "min")) {
		alarm_min = val;
	} else if (!strcmp(key, "enabled")) {
		alarm_enabled = (val != 0);
	} else if (!strcmp(key, "clock_24h")) {
		clock_24h = (val != 0);
	} else if (!strcmp(key, "tz")) {
		tz_offset = val;
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(device_alarm, "alarm", NULL,
				alarm_settings_set, NULL, NULL);

static void save_alarm_locked(void)
{
	/* ui_state_mutex held by caller */
	if (!settings_ready) {
		return;
	}
	int32_t v;

	v = alarm_hour;    settings_save_one("alarm/hour",    &v, sizeof(v));
	v = alarm_min;     settings_save_one("alarm/min",     &v, sizeof(v));
	v = alarm_enabled; settings_save_one("alarm/enabled", &v, sizeof(v));
	v = clock_24h;     settings_save_one("alarm/clock_24h", &v, sizeof(v));
	v = tz_offset;     settings_save_one("alarm/tz",      &v, sizeof(v));
}

/* =========================================================================
 * Log helpers
 * ========================================================================= */

static void fmt_ts(int64_t uptime_s, int64_t realtime_s,
		   char *out, size_t outlen)
{
	if (realtime_s > 0) {
		time_t t = (time_t)realtime_s;
		struct tm tm;

		gmtime_r(&t, &tm);
		snprintf(out, outlen, "%02d:%02d:%02d",
			 tm.tm_hour, tm.tm_min, tm.tm_sec);
	} else {
		snprintf(out, outlen, "T+%llds", (long long)uptime_s);
	}
}

void device_ui_log_push(const char *msg)
{
	int64_t uptime_s   = k_uptime_get() / 1000;
	int64_t realtime_s = 0;

	if (sys_flag_get() & SYS_FLAG_TIME_VALID) {
		struct timespec ts;

		if (sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) == 0) {
			realtime_s = ts.tv_sec;
		}
	}

	k_mutex_lock(&log_mutex, K_FOREVER);

	struct log_entry *e = &log_buf[log_next];

	e->uptime_s   = uptime_s;
	e->realtime_s = realtime_s;
	strncpy(e->msg, msg, LOG_MSG_LEN - 1);
	e->msg[LOG_MSG_LEN - 1] = '\0';

	log_next = (log_next + 1) % LOG_ENTRIES;
	if (log_count < LOG_ENTRIES) {
		log_count++;
	}

	k_mutex_unlock(&log_mutex);

	if ((enum dev_screen)atomic_get(&active_scr) == SCR_LOG) {
		signal_render();
	}
}

/* =========================================================================
 * Render functions — called from device_ui_thread with lvgl_mutex held
 * ========================================================================= */

static void render_log_locked(void)
{
	int pos = 0;
	char ts[LOG_TS_LEN + 2];

	k_mutex_lock(&log_mutex, K_FOREVER);

	int n = log_count;
	int start = (log_count < LOG_ENTRIES) ? 0 : log_next;

	for (int i = 0; i < n && pos < LOG_TEXT_MAX; i++) {
		int idx = (start + i) % LOG_ENTRIES;
		const struct log_entry *e = &log_buf[idx];

		fmt_ts(e->uptime_s, e->realtime_s, ts, sizeof(ts));
		int w = snprintf(log_text_buf + pos, LOG_TEXT_MAX - pos,
				 "%s > %s\n", ts, e->msg);
		if (w > 0) {
			pos += w;
		}
	}

	k_mutex_unlock(&log_mutex);

	/* Trim trailing newline */
	if (pos > 0 && log_text_buf[pos - 1] == '\n') {
		log_text_buf[pos - 1] = '\0';
	} else {
		log_text_buf[pos] = '\0';
	}

	lv_label_set_text(log_content, n ? log_text_buf : "(no messages)");
	lv_screen_load(clk_scr);
}

static void render_net_locked(void)
{
	const char *ssid = wifi_prov_get_ssid();
	const char *ip   = wifi_prov_get_ip();

	lv_label_set_text(net_ssid_lbl, (ssid && ssid[0]) ? ssid : "Not connected");
	lv_label_set_text(net_ip_lbl,   (ip && ip[0]) ? ip : "—");
	lv_screen_load(net_scr);
}

static const char *const wday_name[7] = {
	"Sunday", "Monday", "Tuesday", "Wednesday",
	"Thursday", "Friday", "Saturday",
};

static const char *const month_name[12] = {
	"January", "February", "March", "April", "May", "June",
	"July", "August", "September", "October", "November", "December",
};

static void format_hhmm(int hour24, int min, bool use_24h,
			char *time_buf, size_t time_len,
			char *suffix_buf, size_t suffix_len)
{
	if (use_24h) {
		snprintf(time_buf, time_len, "%02d:%02d", hour24, min);
		suffix_buf[0] = '\0';
		return;
	}

	int hour12 = hour24 % 12;

	if (hour12 == 0) {
		hour12 = 12;
	}

	snprintf(time_buf, time_len, "%02d:%02d", hour12, min);
	strncpy(suffix_buf, (hour24 < 12) ? "AM" : "PM", suffix_len - 1);
	suffix_buf[suffix_len - 1] = '\0';
}

static void format_tz_label(char *buf, size_t len)
{
	snprintf(buf, len, "UTC%+d", tz_offset);
}

static void format_alarm_line(char *buf, size_t len)
{
	char time_buf[8];
	char suffix[4];

	format_hhmm(alarm_hour, alarm_min, clock_24h,
		    time_buf, sizeof(time_buf), suffix, sizeof(suffix));
	snprintf(buf, len, "Alarm %s%s%s  %s",
		 time_buf,
		 suffix[0] ? " " : "",
		 suffix,
		 alarm_enabled ? "ON" : "OFF");
}

static void update_clock_status_locked(void)
{
	uint32_t flags = sys_flag_get();
	char status_buf[24] = "";

	if (flags & SYS_FLAG_WIFI_READY) {
		strncpy(status_buf, LV_SYMBOL_WIFI, sizeof(status_buf) - 1);
		status_buf[sizeof(status_buf) - 1] = '\0';
		if (flags & SYS_FLAG_LLSS_AUTHORIZED) {
			strncat(status_buf, " ", sizeof(status_buf) - strlen(status_buf) - 1);
			strncat(status_buf, LV_SYMBOL_OK,
				sizeof(status_buf) - strlen(status_buf) - 1);
		}
	}

	lv_label_set_text(clk_status_lbl, status_buf);
	lv_label_set_text(clk_power_lbl, "");
}

static void render_clk_locked(void)
{
	char time_buf[8];
	char date_buf[48];
	char suffix[4];

	struct timespec ts = {0};
	struct tm tm      = {0};
	bool have_time = (sys_flag_get() & SYS_FLAG_TIME_VALID) &&
			 sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) == 0;

	if (have_time) {
		time_t local = ts.tv_sec + (time_t)(tz_offset * 3600);

		gmtime_r(&local, &tm);
		format_hhmm(tm.tm_hour, tm.tm_min, clock_24h,
			    time_buf, sizeof(time_buf), suffix, sizeof(suffix));
		snprintf(date_buf, sizeof(date_buf), "%s, %d %s %d%s%s",
			 wday_name[tm.tm_wday % 7], tm.tm_mday,
			 month_name[tm.tm_mon % 12], tm.tm_year + 1900,
			 suffix[0] ? "  " : "", suffix);
	} else {
		strncpy(time_buf, "--:--", sizeof(time_buf));
		strncpy(date_buf, "Waiting for time sync...", sizeof(date_buf));
	}

	lv_label_set_text(clk_date_lbl,  date_buf);
	lv_label_set_text(clk_time_lbl, time_buf);
	update_clock_status_locked();
	lv_screen_load(clk_scr);
}

static void render_alarm_locked(void)
{
	char time_buf[8];
	char suffix[4];
	char date_buf[48];
	char meta_buf[32];
	char alarm_buf[48];
	char tz_buf[12];
	struct timespec ts = {0};
	struct tm tm = {0};
	bool have_time = (sys_flag_get() & SYS_FLAG_TIME_VALID) &&
			 sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) == 0;

	if (have_time) {
		time_t local = ts.tv_sec + (time_t)(tz_offset * 3600);

		gmtime_r(&local, &tm);
		format_hhmm(tm.tm_hour, tm.tm_min, clock_24h,
			    time_buf, sizeof(time_buf), suffix, sizeof(suffix));
		snprintf(date_buf, sizeof(date_buf), "%s, %d %s %d",
			 wday_name[tm.tm_wday % 7], tm.tm_mday,
			 month_name[tm.tm_mon % 12], tm.tm_year + 1900);
	} else {
		strncpy(time_buf, "--:--", sizeof(time_buf));
		suffix[0] = '\0';
		strncpy(date_buf, "Waiting for time sync...", sizeof(date_buf));
	}

	format_tz_label(tz_buf, sizeof(tz_buf));
	format_alarm_line(alarm_buf, sizeof(alarm_buf));
	snprintf(meta_buf, sizeof(meta_buf), "%s  |  %s%s%s",
		 tz_buf, clock_24h ? "24H" : "12H",
		 suffix[0] ? "  " : "",
		 suffix);

	lv_label_set_text(alm_date_lbl, date_buf);
	lv_label_set_text(alm_time_lbl, time_buf);
	lv_label_set_text(alm_meta_lbl, meta_buf);
	lv_label_set_text(alm_alarm_lbl, alarm_buf);
	lv_screen_load(alm_scr);
}

static void device_ui_render_locked(void)
{
	switch ((enum dev_screen)atomic_get(&active_scr)) {
	case SCR_LOG:     render_log_locked();               break;
	case SCR_NETWORK: render_net_locked();               break;
	case SCR_CLOCK:   render_clk_locked();               break;
	case SCR_ALARM:   render_alarm_locked();             break;
	case SCR_MAIN:    lv_screen_load(main_scr_ref);      break;
	}
}

/* =========================================================================
 * Input handler
 * ========================================================================= */

/* Adjust alarm/timezone state from a clock-screen button.  Returns true if
 * any value changed (caller persists + re-renders).  ui_state_mutex held. */
static bool alarm_apply_btn_locked(enum ui_btn btn)
{
	switch (btn) {
	case UI_BTN_1: alarm_hour = (alarm_hour + 1) % 24;  return true;
	case UI_BTN_2: alarm_hour = (alarm_hour + 23) % 24; return true;
	case UI_BTN_3: alarm_min  = (alarm_min + 1) % 60;   return true;
	case UI_BTN_4: alarm_min  = (alarm_min + 59) % 60;  return true;
	case UI_BTN_5: alarm_enabled = !alarm_enabled;      return true;
	case UI_BTN_6: clock_24h = !clock_24h;              return true;
	case UI_BTN_7: if (tz_offset < 14)  { tz_offset++; return true; } break;
	case UI_BTN_8: if (tz_offset > -12) { tz_offset--; return true; } break;
	default: break;
	}
	return false;
}

bool device_ui_handle_input(enum ui_btn btn, enum ui_evt evt)
{
	enum dev_screen scr = (enum dev_screen)atomic_get(&active_scr);

	/* From the main (server) screen, only the trigger is intercepted. */
	if (scr == SCR_MAIN) {
		if (btn == UI_BTN_ENTER && evt == UI_EVT_LONG_PRESS) {
			atomic_set(&active_scr, SCR_LOG);
			signal_render();
			return true;
		}
		return false;
	}

	/* Device menu is open — every button below is consumed. */
	if (evt != UI_EVT_PRESS) {
		return true;
	}

	/* ESC exits to the main (server) screen */
	if (btn == UI_BTN_ESC) {
		atomic_set(&active_scr, SCR_MAIN);
		signal_render();
		return true;
	}

	/* HL_LEFT / HL_RIGHT cycle screens */
	if (btn == UI_BTN_HL_RIGHT || btn == UI_BTN_HL_LEFT) {
		enum dev_screen next;

		if (btn == UI_BTN_HL_RIGHT) {
			next = (scr == SCR_LOG)     ? SCR_NETWORK :
			       (scr == SCR_NETWORK) ? SCR_CLOCK   :
			       (scr == SCR_CLOCK)   ? SCR_ALARM   : SCR_LOG;
		} else {
			next = (scr == SCR_LOG)     ? SCR_ALARM   :
			       (scr == SCR_NETWORK) ? SCR_LOG     :
			       (scr == SCR_CLOCK)   ? SCR_NETWORK : SCR_CLOCK;
		}
		atomic_set(&active_scr, next);
		signal_render();
		return true;
	}

	/* Screen-specific context actions */
	switch (scr) {
	case SCR_LOG:
		if (btn == UI_BTN_1) {
			k_mutex_lock(&log_mutex, K_FOREVER);
			log_next  = 0;
			log_count = 0;
			k_mutex_unlock(&log_mutex);
			signal_render();
		}
		break;

	case SCR_NETWORK:
		if (btn == UI_BTN_1) {
			/* Soft recovery: start AP without wiping credentials */
			wifi_prov_start_ap();
			atomic_set(&active_scr, SCR_LOG);
			signal_render();
		} else if (btn == UI_BTN_2) {
			/* Hard reset: wipe credentials + cold reboot */
			device_ui_log_push("Forgetting WiFi credentials...");
			wifi_prov_clear_credentials();
			k_msleep(100);
			sys_reboot(SYS_REBOOT_COLD);
		}
		break;

	case SCR_ALARM: {
		k_mutex_lock(&ui_state_mutex, K_FOREVER);
		bool dirty = alarm_apply_btn_locked(btn);

		if (dirty) {
			save_alarm_locked();
		}
		k_mutex_unlock(&ui_state_mutex);

		if (dirty) {
			signal_render();
		}
		break;
	}

	default:
		break;
	}

	return true; /* consume all input while the device menu is open */
}

bool device_ui_is_active(void)
{
	return (enum dev_screen)atomic_get(&active_scr) != SCR_MAIN;
}

/* =========================================================================
 * Alarm check — called from the render thread (no mutex needed for read)
 * ========================================================================= */

static void check_alarm(void)
{
	if (!alarm_enabled || !(sys_flag_get() & SYS_FLAG_TIME_VALID)) {
		return;
	}

	struct timespec ts = {0};

	if (sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) != 0) {
		return;
	}

	time_t local = ts.tv_sec + (time_t)(tz_offset * 3600);
	struct tm tm;

	gmtime_r(&local, &tm);

	bool match = (tm.tm_hour == alarm_hour && tm.tm_min == alarm_min);

	if (match && !alarm_fired) {
		alarm_fired = true;
		device_ui_log_push("*** ALARM ***");
		LOG_WRN("Alarm fired at %02d:%02d local", alarm_hour, alarm_min);
	} else if (!match) {
		alarm_fired = false;
	}
}

/* =========================================================================
 * Theme + layout
 *
 * 800x480, 4-gray panel (full refresh).  The driver dithers UI screens, so
 * intermediate grays render as believable shades.  Layout is a fixed three-
 * band grid: dark header, white content, soft-key footer whose 8 cells sit
 * directly under the physical BTN_1..8 below the screen.
 * ========================================================================= */

#define SCR_W      800
#define SCR_H      480
#define HDR_H      64
#define FTR_H      72
#define CONTENT_Y  HDR_H
#define CLOCK_CENTER_Y_OFF (HDR_H / 2)

static lv_color_t c_ink(void)   { return lv_color_hex(0x000000); }
static lv_color_t c_paper(void) { return lv_color_hex(0xffffff); }
static lv_color_t c_gray(uint8_t v)
{
	return lv_color_hex(((uint32_t)v << 16) | ((uint32_t)v << 8) | v);
}

/* Bare rectangle child — no border/padding/scroll, opaque fill. */
static lv_obj_t *panel(lv_obj_t *parent, int w, int h)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_remove_style_all(o);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_size(o, w, h);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	return o;
}

static lv_obj_t *new_screen(void)
{
	lv_obj_t *s = lv_obj_create(NULL);

	lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(s, c_paper(), 0);
	lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
	return s;
}

/* Dark title bar with left title + subtle top-button capsules. */
static void build_header(lv_obj_t *scr, const char *title)
{
	static const char *const top_labels[8] = {
		NULL, NULL, NULL, NULL, "<", ">", "ENT", "ESC"
	};
	const int cw = SCR_W / 8;
	const int gap = 6;
	const int slot_h = 24;
	lv_obj_t *bar = panel(scr, SCR_W, HDR_H);

	lv_obj_set_pos(bar, 0, 0);
	lv_obj_set_style_bg_color(bar, c_ink(), 0);

	lv_obj_t *t = lv_label_create(bar);

	lv_obj_set_style_text_color(t, c_paper(), 0);
	lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
	lv_label_set_text(t, title);
	lv_obj_align(t, LV_ALIGN_LEFT_MID, 20, 0);

	for (int i = 0; i < 8; i++) {
		const char *label = top_labels[i];
		lv_obj_t *slot;

		if (!label) {
			continue;
		}

		slot = panel(bar, cw - gap, slot_h);

		lv_obj_set_style_bg_color(slot, c_ink(), 0);
		lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
		lv_obj_set_style_border_width(slot, 1, 0);
		lv_obj_set_style_border_color(slot, c_gray(0x58), 0);
		lv_obj_set_style_radius(slot, 4, 0);
		lv_obj_set_pos(slot, i * cw + gap / 2, (HDR_H - slot_h) / 2);

		lv_obj_t *txt = lv_label_create(slot);

		lv_obj_set_style_text_color(txt, c_gray(0xB8), 0);
		lv_obj_set_style_text_font(txt, &lv_font_montserrat_14, 0);
		lv_label_set_text(txt, label);
		lv_obj_center(txt);
	}
}

/* Bottom strip of 8 key-caps aligned under the physical BTN_1..8. */
static void build_softkeys(lv_obj_t *scr, const char *const labels[8])
{
	const int cw  = SCR_W / 8; /* 100 px per key */
	const int gap = 6;

	lv_obj_t *strip = panel(scr, SCR_W, FTR_H);

	lv_obj_align(strip, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_color(strip, c_paper(), 0);

	for (int i = 0; i < 8; i++) {
		bool used = labels[i] && labels[i][0];
		lv_obj_t *cap = panel(strip, cw - gap, FTR_H - gap * 2);

		lv_obj_set_pos(cap, i * cw + gap / 2, gap);
		lv_obj_set_style_radius(cap, 8, 0);
		lv_obj_set_style_bg_color(cap, used ? c_ink() : c_gray(0xC8), 0);

		if (!used) {
			continue;
		}

		lv_obj_t *l = lv_label_create(cap);

		lv_obj_set_style_text_color(l, c_paper(), 0);
		lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
		lv_obj_set_width(l, cw - gap - 8);
		lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
		lv_label_set_text(l, labels[i]);
		lv_obj_center(l);
	}
}

/* =========================================================================
 * LVGL screen construction — called from device_ui_init (lvgl_mutex held)
 * ========================================================================= */

static void build_log_scr(void)
{
	static const char *const keys[8] = { "Clear", 0, 0, 0, 0, 0, 0, 0 };

	log_scr = new_screen();
	build_header(log_scr, "System Log");
	build_softkeys(log_scr, keys);

	log_content = lv_label_create(log_scr);
	lv_obj_set_style_text_color(log_content, c_ink(), 0);
	lv_obj_set_style_text_font(log_content, &lv_font_montserrat_14, 0);
	lv_obj_set_size(log_content, SCR_W - 32, SCR_H - HDR_H - FTR_H - 16);
	lv_label_set_long_mode(log_content, LV_LABEL_LONG_WRAP);
	lv_obj_set_pos(log_content, 16, CONTENT_Y + 8);
	lv_label_set_text(log_content, "Booting...");
}

static void build_field(lv_obj_t *scr, const char *caption, int y,
			lv_obj_t **value_out)
{
	lv_obj_t *cap = lv_label_create(scr);

	lv_obj_set_style_text_color(cap, c_gray(0x66), 0);
	lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
	lv_label_set_text(cap, caption);
	lv_obj_set_pos(cap, 40, y);

	lv_obj_t *val = lv_label_create(scr);

	lv_obj_set_style_text_color(val, c_ink(), 0);
	lv_obj_set_style_text_font(val, &lv_font_montserrat_28, 0);
	lv_label_set_text(val, "—");
	lv_obj_set_pos(val, 40, y + 22);
	*value_out = val;
}

static void build_net_scr(void)
{
	static const char *const keys[8] = {
		"AP Mode", "Forget WiFi", 0, 0, 0, 0, 0, 0
	};

	net_scr = new_screen();
	build_header(net_scr, "Network");
	build_softkeys(net_scr, keys);

	build_field(net_scr, "WI-FI NETWORK", CONTENT_Y + 40, &net_ssid_lbl);
	build_field(net_scr, "IP ADDRESS",    CONTENT_Y + 130, &net_ip_lbl);
}

static void build_clk_scr(void)
{
	clk_scr = new_screen();
	build_header(clk_scr, "Clock");

	clk_date_lbl = lv_label_create(clk_scr);
	lv_obj_set_width(clk_date_lbl, SCR_W);
	lv_obj_set_style_text_color(clk_date_lbl, c_gray(0x66), 0);
	lv_obj_set_style_text_font(clk_date_lbl, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_align(clk_date_lbl, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(clk_date_lbl, "—");

	clk_time_bg_lbl = lv_label_create(clk_scr);
	lv_obj_set_style_text_color(clk_time_bg_lbl, c_gray(0xD0), 0);
	lv_obj_set_style_text_opa(clk_time_bg_lbl, LV_OPA_70, 0);
	lv_obj_set_style_text_font(clk_time_bg_lbl, &dseg_bold_italic_200, 0);
	lv_obj_set_style_text_letter_space(clk_time_bg_lbl, 0, 0);
	lv_label_set_text(clk_time_bg_lbl, CLK_GHOST_TEXT);
	lv_obj_align(clk_time_bg_lbl, LV_ALIGN_CENTER, 0, CLOCK_CENTER_Y_OFF);
	lv_obj_update_layout(clk_scr);
	lv_obj_align(clk_date_lbl, LV_ALIGN_TOP_MID, 0,
		lv_obj_get_y(clk_time_bg_lbl) - lv_obj_get_height(clk_date_lbl) - 22);

	clk_time_lbl = lv_label_create(clk_scr);
	lv_obj_set_style_text_color(clk_time_lbl, c_ink(), 0);
	lv_obj_set_style_text_font(clk_time_lbl, &dseg_bold_italic_200, 0);
	lv_obj_set_style_text_letter_space(clk_time_lbl, 0, 0);
	lv_label_set_text(clk_time_lbl, "--:--");
	lv_obj_align_to(clk_time_lbl, clk_time_bg_lbl, LV_ALIGN_CENTER, 0, 0);

	clk_status_lbl = lv_label_create(clk_scr);
	lv_obj_set_style_text_color(clk_status_lbl, c_ink(), 0);
	lv_obj_set_style_text_font(clk_status_lbl, &lv_font_montserrat_14, 0);
	lv_label_set_text(clk_status_lbl, "");
	lv_obj_align(clk_status_lbl, LV_ALIGN_TOP_RIGHT, -24, CONTENT_Y + 14);

	clk_power_lbl = lv_label_create(clk_scr);
	lv_obj_set_style_text_color(clk_power_lbl, c_gray(0x66), 0);
	lv_obj_set_style_text_font(clk_power_lbl, &lv_font_montserrat_14, 0);
	lv_label_set_text(clk_power_lbl, "");
	lv_obj_align(clk_power_lbl, LV_ALIGN_TOP_RIGHT, -94, CONTENT_Y + 14);
}

static void build_alarm_scr(void)
{
	static const char *const keys[8] = {
		"Hour +", "Hour -", "Min +", "Min -",
		"Alarm On/Off", "24/12", "TZ +", "TZ -"
	};

	alm_scr = new_screen();
	build_header(alm_scr, "Alarm");
	build_softkeys(alm_scr, keys);

	alm_date_lbl = lv_label_create(alm_scr);
	lv_obj_set_style_text_color(alm_date_lbl, c_gray(0x66), 0);
	lv_obj_set_style_text_font(alm_date_lbl, &lv_font_montserrat_28, 0);
	lv_label_set_text(alm_date_lbl, "—");
	lv_obj_align(alm_date_lbl, LV_ALIGN_TOP_MID, 0, CONTENT_Y + 24);

	alm_time_lbl = lv_label_create(alm_scr);
	lv_obj_set_style_text_color(alm_time_lbl, c_ink(), 0);
	lv_obj_set_style_text_font(alm_time_lbl, &lv_font_montserrat_48, 0);
	lv_label_set_text(alm_time_lbl, "--:--");
	lv_obj_align(alm_time_lbl, LV_ALIGN_CENTER, 0, -34);

	alm_meta_lbl = lv_label_create(alm_scr);
	lv_obj_set_style_text_color(alm_meta_lbl, c_gray(0x66), 0);
	lv_obj_set_style_text_font(alm_meta_lbl, &lv_font_montserrat_14, 0);
	lv_label_set_text(alm_meta_lbl, "UTC+0  |  24H");
	lv_obj_align(alm_meta_lbl, LV_ALIGN_CENTER, 0, 20);

	alm_alarm_lbl = lv_label_create(alm_scr);
	lv_obj_set_style_text_color(alm_alarm_lbl, c_ink(), 0);
	lv_obj_set_style_text_font(alm_alarm_lbl, &lv_font_montserrat_28, 0);
	lv_label_set_text(alm_alarm_lbl, "Alarm --:-- ON");
	lv_obj_align(alm_alarm_lbl, LV_ALIGN_BOTTOM_MID, 0, -FTR_H - 24);
}

/* =========================================================================
 * Render thread
 * ========================================================================= */

#define DEVICE_UI_STACK    4096
#define DEVICE_UI_PRIORITY 14

/* A burst of rapid log pushes (e.g. during boot) must not each trigger a
 * separate ~2 s full e-paper refresh.  After a signal wakes us, settle for
 * this long so the burst collapses into a single render. */
#define UI_COALESCE_MS     350

/* How long to wait when we have no real-time clock yet (retry the tick). */
#define UI_TICK_NO_TIME_MS 30000

/* Timeout until the next render tick.  On a valid clock we wake just after the
 * minute boundary (the clock shows HH:MM, so per-minute is the right cadence
 * for e-paper); alarm checks ride the same tick.  Without a clock we poll. */
static k_timeout_t ui_next_tick(void)
{
	struct timespec ts;

	if ((sys_flag_get() & SYS_FLAG_TIME_VALID) &&
	    sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) == 0) {
		int into_min_ms = (int)(ts.tv_sec % 60) * 1000 +
				  (int)(ts.tv_nsec / 1000000);
		int to_boundary = 60000 - into_min_ms + 100; /* +100ms margin */

		return K_MSEC(to_boundary);
	}
	return K_MSEC(UI_TICK_NO_TIME_MS);
}

static void device_ui_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	struct k_poll_event evt = K_POLL_EVENT_INITIALIZER(
		K_POLL_TYPE_SIGNAL,
		K_POLL_MODE_NOTIFY_ONLY,
		&ui_signal);

	while (true) {
		int rc = k_poll(&evt, 1, ui_next_tick());
		bool signaled = (rc == 0);

		k_poll_signal_reset(&ui_signal);
		evt.state = K_POLL_STATE_NOT_READY;

		if (signaled) {
			/* Collapse a burst of updates into one refresh. */
			k_sleep(K_MSEC(UI_COALESCE_MS));
			k_poll_signal_reset(&ui_signal);
			evt.state = K_POLL_STATE_NOT_READY;
		}

		check_alarm();

		/* A bare clock tick only needs to repaint the time-bearing screens;
		 * other screens are static until the next input signal. */
		enum dev_screen scr = (enum dev_screen)atomic_get(&active_scr);
		bool is_clock = (scr == SCR_CLOCK || scr == SCR_ALARM);

		if (!signaled && !is_clock) {
			continue;
		}

		k_mutex_lock(&lvgl_mutex, K_FOREVER);
		device_ui_render_locked();

		/* Full refresh only when the screen actually changes. An in-place
		 * update (clock tick, new log line) is a fast partial — the panel
		 * diffs the framebuffer itself, so it doesn't matter that LVGL
		 * repaints the whole screen. */
		static enum dev_screen prev_scr = SCR_MAIN;
		bool switched = (scr != prev_scr);

		prev_scr = scr;

		/* Dither only screens that use real gray (clock/alarm ghost
		 * segments); pure-text screens stay crisp B/W since dithering
		 * muddies small anti-aliased text. */
		bool dither = (scr == SCR_CLOCK || scr == SCR_ALARM);

		ui_lvgl_flush(dither, switched ? UI_CTX_SWITCH : UI_CTX_UI);
		k_mutex_unlock(&lvgl_mutex);
	}
}

K_THREAD_DEFINE(device_ui_thread, DEVICE_UI_STACK,
		device_ui_thread_fn, NULL, NULL, NULL,
		DEVICE_UI_PRIORITY, 0, 0);

/* =========================================================================
 * Public API
 * ========================================================================= */

void device_ui_init(lv_obj_t *main_scr)
{
	main_scr_ref = main_scr;

	/* lvgl_mutex already held by ui_init() caller */
	build_log_scr();
	build_net_scr();
	build_clk_scr();
	build_alarm_scr();

	lv_screen_load(log_scr);
}

void device_ui_show_main(void)
{
	/* Called with lvgl_mutex held by the display thread.
	 * Just update the atomic; the caller loads the screen itself. */
	atomic_set(&active_scr, SCR_MAIN);
}

void device_ui_settings_ready(void)
{
	k_mutex_lock(&ui_state_mutex, K_FOREVER);
	settings_ready = true;
	k_mutex_unlock(&ui_state_mutex);
}
