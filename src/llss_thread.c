/*
 * LLSS thread — state machine + API calls
 *
 * Single thread that owns all LLSS protocol logic: registration,
 * authentication, token refresh, polling, frame fetching, and button
 * input forwarding.  The TLS session is kept alive across consecutive
 * API calls and torn down only on network errors or WiFi loss.
 *
 * The display thread is the only other thread that runs alongside this one.
 * All LVGL interaction goes through display_thread.h.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include "device_ui.h"
#include "display_thread.h"
#include "input_events.h"
#include "llss_client.h"
#include "llss_storage.h"
#include "llss_thread.h"
#include "material_icons.h"
#include "system_flags.h"
#include "wifi_prov.h"

LOG_MODULE_REGISTER(llss, LOG_LEVEL_INF);

#define LLSS_THREAD_STACK    16384
#define LLSS_THREAD_PRIORITY 10

/* =========================================================================
 * State machine
 * ========================================================================= */

enum app_state {
	STATE_BOOTING = 0,
	STATE_REGISTERING,
	STATE_WAITING_AUTHORIZATION,
	STATE_REFRESHING,
	STATE_AUTHENTICATING,
	STATE_POLLING,
	STATE_FETCHING_FRAME,
	STATE_SLEEPING,
	STATE_ERROR,
};

static volatile enum app_state app_state = STATE_BOOTING;

/* =========================================================================
 * Credentials (LLSS thread only)
 * ========================================================================= */

static char device_id[LLSS_DEVICE_ID_MAX];
static char device_secret[LLSS_DEVICE_SECRET_MAX];
static char refresh_token[LLSS_TOKEN_MAX];
static char access_token[LLSS_TOKEN_MAX];
static char hardware_id[16];
static char last_frame_id[64];

static int32_t poll_interval_ms = CONFIG_LLSS_POLL_INTERVAL_MS;

/* =========================================================================
 * Button input
 * ========================================================================= */

#define LONG_PRESS_MS 500

struct button_event {
	enum ui_btn btn;
	enum ui_evt evt;
};

K_MSGQ_DEFINE(btn_queue, sizeof(struct button_event), 8, 4);

/* Mapped keys = 12 (8 BTN_* + ENTER + ESC + HL_LEFT + HL_RIGHT).  Unmapped
 * codes — including the 6 IO-expander buttons reserved for device-local
 * config — never reach the slot table and are silently dropped. */
#define KEY_SLOTS 12

/* Per-key state machine.  LONG_PRESS fires from a delayable work after
 * LONG_PRESS_MS of continuous hold; release that arrives before the work
 * runs fires PRESS instead.  Atomic CAS arbitrates the race between work
 * handler and release event. */
enum key_state {
	KEY_IDLE = 0,
	KEY_PRESSED,
	KEY_LONG_FIRED,
};

struct key_slot {
	int32_t                   code;     /* 0 = free */
	atomic_t                  state;
	struct k_work_delayable   long_work;
};

static struct key_slot key_slots[KEY_SLOTS];

static struct key_slot *find_slot(int32_t code)
{
	struct key_slot *free_slot = NULL;

	for (int32_t i = 0; i < KEY_SLOTS; i++) {
		if (key_slots[i].code == code) {
			return &key_slots[i];
		}
		if (!free_slot && key_slots[i].code == 0) {
			free_slot = &key_slots[i];
		}
	}
	if (free_slot) {
		free_slot->code = code;
	}
	return free_slot;
}

static void enqueue_event(int32_t code, enum ui_evt evt)
{
	enum ui_btn btn = ui_btn_from_keycode(code);

	if (btn == UI_BTN_NONE) {
		return;
	}

	/* Device UI gets first crack — handles trigger + all menu events */
	if (device_ui_handle_input(btn, evt)) {
		return;
	}

	struct button_event bev = { .btn = btn, .evt = evt };

	k_msgq_put(&btn_queue, &bev, K_NO_WAIT);
}

static void long_press_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct key_slot *slot = CONTAINER_OF(dwork, struct key_slot, long_work);

	/* CAS guards against a release that beat us to the IDLE transition. */
	if (atomic_cas(&slot->state, KEY_PRESSED, KEY_LONG_FIRED)) {
		enqueue_event(slot->code, UI_EVT_LONG_PRESS);
	}
}

static int key_slots_init(void)
{
	for (int32_t i = 0; i < KEY_SLOTS; i++) {
		k_work_init_delayable(&key_slots[i].long_work, long_press_handler);
		atomic_set(&key_slots[i].state, KEY_IDLE);
	}
	return 0;
}
SYS_INIT(key_slots_init, APPLICATION, 50);

static void on_input(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY) {
		return;
	}

	if (ui_btn_from_keycode(evt->code) == UI_BTN_NONE) {
		return;
	}

	struct key_slot *slot = find_slot(evt->code);

	if (!slot) {
		return;
	}

	if (evt->value != 0) {
		/* Press down — arm the long-press timer.  reschedule (vs
		 * schedule) handles a stray re-press without an intervening
		 * release by restarting the count. */
		atomic_set(&slot->state, KEY_PRESSED);
		k_work_reschedule(&slot->long_work, K_MSEC(LONG_PRESS_MS));
		return;
	}

	/* Release.  Cancel a still-pending long-press work; whoever wins the
	 * CAS owns the event.  If we win PRESSED→IDLE the work hadn't fired
	 * yet — emit PRESS.  Otherwise it already fired LONG_PRESS and we
	 * drop the release silently. */
	k_work_cancel_delayable(&slot->long_work);

	if (atomic_cas(&slot->state, KEY_PRESSED, KEY_IDLE)) {
		enqueue_event(slot->code, UI_EVT_PRESS);
	} else {
		atomic_set(&slot->state, KEY_IDLE);
	}
}

INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);

/* =========================================================================
 * Shell — inject button events through the input subsystem
 *
 * Goes through input_report_key() so the same on_input() callback runs as
 * for the physical IO-expander buttons.  Useful for bring-up before the
 * expander is wired and for reproducing protocol issues from a console.
 * ========================================================================= */

static int shell_btn(const struct shell *sh, size_t argc, char **argv,
		     bool long_press)
{
	if (argc < 2) {
		shell_error(sh, "Usage: btn %s <NAME>", long_press ? "long" : "press");
		shell_print(sh, "  NAME: BTN_1..BTN_8, ENTER, ESC, HL_LEFT, HL_RIGHT");
		return -EINVAL;
	}

	int32_t code = ui_btn_to_keycode(ui_btn_from_name(argv[1]));

	if (code < 0) {
		shell_error(sh, "Unknown button: %s", argv[1]);
		return -EINVAL;
	}

	int32_t rc = input_report_key(NULL, code, 1, true, K_FOREVER);

	if (rc < 0) {
		shell_error(sh, "input_report_key press failed: %d", rc);
		return rc;
	}

	k_msleep(long_press ? (LONG_PRESS_MS + 100) : 50);

	rc = input_report_key(NULL, code, 0, true, K_FOREVER);
	if (rc < 0) {
		shell_error(sh, "input_report_key release failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Injected %s on %s",
		    long_press ? "LONG_PRESS" : "PRESS", argv[1]);
	return 0;
}

static int cmd_btn_press(const struct shell *sh, size_t argc, char **argv)
{
	return shell_btn(sh, argc, argv, false);
}

static int cmd_btn_long(const struct shell *sh, size_t argc, char **argv)
{
	return shell_btn(sh, argc, argv, true);
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_btn,
	SHELL_CMD_ARG(press, NULL,
		      "Short press: btn press <BTN_1..8|ENTER|ESC|HL_LEFT|HL_RIGHT>",
		      cmd_btn_press, 2, 0),
	SHELL_CMD_ARG(long,  NULL,
		      "Long press:  btn long  <BTN_1..8|ENTER|ESC|HL_LEFT|HL_RIGHT>",
		      cmd_btn_long, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(btn, &sub_btn,
		   "Inject button events through the input subsystem", NULL);

/* =========================================================================
 * WiFi / session
 * ========================================================================= */

/* WiFi readiness is signalled via SYS_FLAG_WIFI_READY (see system_flags.h),
 * NTP time via SYS_FLAG_TIME_VALID, authorization via SYS_FLAG_LLSS_AUTHORIZED.
 * Producers set/clear; the main loop waits on the bits it needs. */
static atomic_t session_reset_requested = ATOMIC_INIT(0);
static bool session_open;

void llss_on_wifi(enum wifi_prov_state state, const char *info)
{
	char msg[80];

	switch (state) {
	case WIFI_PROV_CONNECTING:
		LOG_INF("WiFi connecting: %s", info);
		snprintf(msg, sizeof(msg), "WiFi connecting: %s", info);
		ui_log_push(msg);
		ui_server_status_show(ICON_WIFI,
			"Connecting to Wi-Fi",
			(info && info[0]) ? info : "Joining the saved network.");
		sys_flag_clear(SYS_FLAG_WIFI_READY | SYS_FLAG_WIFI_PROVISIONING);
		break;
	case WIFI_PROV_CONNECTED:
		LOG_INF("WiFi connected: %s", info);
		snprintf(msg, sizeof(msg), "WiFi connected — IP: %s", info);
		ui_log_push(msg);
		if (!last_frame_id[0]) {
			ui_server_status_show(ICON_SCHEDULE,
				"Connected",
				"Syncing time and preparing your first screen.");
		}
		sys_flag_clear(SYS_FLAG_WIFI_PROVISIONING);
		sys_flag_set(SYS_FLAG_WIFI_READY);
		break;
	case WIFI_PROV_DISCONNECTED:
		LOG_WRN("WiFi disconnected");
		ui_log_push("WiFi disconnected — reconnecting...");
		if (!last_frame_id[0]) {
			ui_server_status_show(ICON_WARNING,
				"Connection lost",
				"Trying to reconnect to Wi-Fi.");
		}
		sys_flag_clear(SYS_FLAG_WIFI_READY | SYS_FLAG_WIFI_PROVISIONING);
		/* Tear down the held TLS session — the underlying TCP is dead.
		 * The next iteration will block on SYS_FLAG_WIFI_READY at the
		 * top of the loop until the link comes back. */
		atomic_set(&session_reset_requested, 1);
		break;
	case WIFI_PROV_AP_ACTIVE:
		snprintf(msg, sizeof(msg), "AP mode — connect to: %s", info);
		ui_log_push(msg);
		ui_server_status_show(ICON_WIFI_HOTSPOT,
			"Set up Wi-Fi",
			(info && info[0]) ? info : "Connect to the device access point to continue.");
		sys_flag_clear(SYS_FLAG_WIFI_READY);
		sys_flag_set(SYS_FLAG_WIFI_PROVISIONING);
		break;
	default:
		break;
	}
}

static void session_check_reset(void)
{
	if (atomic_cas(&session_reset_requested, 1, 0) && session_open) {
		llss_session_close();
		session_open = false;
	}
}

static int session_ensure(void)
{
	if (session_open) {
		return 0;
	}

	int32_t rc = llss_session_open();

	if (rc == 0) {
		session_open = true;
	}
	return rc;
}

static void session_close_on_net_error(int rc)
{
	/* Auth (-EACCES) and credential (-ENOENT) errors leave the TCP
	 * connection intact; only network-level failures need a new session. */
	if (rc < 0 && rc != -EACCES && rc != -ENOENT) {
		llss_session_close();
		session_open = false;
	}
}

/* =========================================================================
 * Hardware ID (MAC address as hex string)
 * ========================================================================= */

static void build_hardware_id(void)
{
	struct net_if *iface = net_if_get_default();

	if (!iface) {
		strncpy(hardware_id, "unknown", sizeof(hardware_id));
		return;
	}

	struct net_linkaddr *ll = net_if_get_link_addr(iface);

	if (!ll || ll->len < 6) {
		strncpy(hardware_id, "unknown", sizeof(hardware_id));
		return;
	}

	snprintf(hardware_id, sizeof(hardware_id),
		 "%02x%02x%02x%02x%02x%02x",
		 ll->addr[0], ll->addr[1], ll->addr[2],
		 ll->addr[3], ll->addr[4], ll->addr[5]);
}

/* =========================================================================
 * Credential helpers
 * ========================================================================= */

static void update_access_token(const char *token)
{
	if (!token || !token[0]) {
		return;
	}
	strncpy(access_token, token, sizeof(access_token) - 1);
	llss_storage_save_access_token(access_token);
}

/* =========================================================================
 * State handlers — each runs one step and returns
 * ========================================================================= */

static enum app_state do_register(void)
{
	ui_log_push("Connecting to server — registering device...");
	ui_server_status_show(ICON_APP_REGISTER,
		"Registering device",
		"Connecting this device to the service.");

	int32_t rc = session_ensure();

	if (rc < 0) {
		LOG_ERR("Session open failed: %d — retry in 5s", rc);
		k_msleep(5000);
		return STATE_REGISTERING;
	}

	char new_id[LLSS_DEVICE_ID_MAX] = {0};
	char new_secret[LLSS_DEVICE_SECRET_MAX] = {0};

	rc = llss_register(hardware_id, new_id, new_secret);
	session_close_on_net_error(rc);

	if (rc == 0) {
		strncpy(device_id, new_id, sizeof(device_id) - 1);
		strncpy(device_secret, new_secret, sizeof(device_secret) - 1);
		llss_storage_save_device(device_id, device_secret);
		LOG_INF("Registered: %s", device_id);
		return STATE_AUTHENTICATING;
	}

	if (rc == 409) {
		/* Server already has a record for this hardware_id.  We don't
		 * hold the device_secret locally (NVS empty or wiped on
		 * reflash), so we can't /auth/devices/token to advance.  Go
		 * to the WAITING state and let either:
		 *   (a) the admin clear the server record, after which our
		 *       next register attempt will succeed, or
		 *   (b) the device be authorized as-is by the admin (we can't
		 *       use it without the secret, but the record is valid). */
		LOG_WRN("Already registered (409) — going to pending.");
		ui_log_push("Device already registered — pending admin approval");
		return STATE_WAITING_AUTHORIZATION;
	}

	LOG_ERR("Registration failed: %d — retry in 10s", rc);
	ui_log_push("Server unreachable — retrying in 10s...");
	k_msleep(10000);
	return STATE_REGISTERING;
}

static enum app_state do_wait_authorization(void)
{
	ui_log_push("Pending authorization - approve device in admin portal");
	ui_server_status_show(ICON_PENDING,
		"Approval required",
		"Authorize this device in the admin portal to continue.");

	/* If we reached WAITING without ever having a device_secret (most
	 * likely we came here from a 409 on /register), there is nothing to
	 * authenticate with.  Sleep, then retry /register — the admin may
	 * have cleared the server-side record in the meantime, which lets
	 * the next register attempt succeed cleanly. */
	if (!device_secret[0]) {
		LOG_INF("No device_secret stored — retrying /register in 30s");
		k_msleep(30000);
		return STATE_REGISTERING;
	}

	int32_t rc = session_ensure();

	if (rc < 0) {
		k_msleep(10000);
		return STATE_WAITING_AUTHORIZATION;
	}

	enum llss_auth_status auth_status;
	char new_refresh[LLSS_TOKEN_MAX] = {0};

	rc = llss_authenticate(hardware_id, device_secret,
			       &auth_status, new_refresh);
	session_close_on_net_error(rc);

	if (rc == 0 && auth_status == LLSS_AUTH_AUTHORIZED && new_refresh[0]) {
		strncpy(refresh_token, new_refresh, sizeof(refresh_token) - 1);
		llss_storage_save_tokens(refresh_token, "");
		LOG_INF("Authorized — got refresh token");
		sys_flag_set(SYS_FLAG_LLSS_AUTHORIZED);
		return STATE_REFRESHING;
	} else if (rc == 0 && auth_status == LLSS_AUTH_PENDING) {
		LOG_INF("Still pending — retry in 15s");
		k_msleep(15000);
		return STATE_WAITING_AUTHORIZATION;
	} else if (auth_status == LLSS_AUTH_REJECTED ||
		   auth_status == LLSS_AUTH_REVOKED || rc == -EACCES) {
		LOG_ERR("Device rejected/revoked");
		sys_flag_clear(SYS_FLAG_LLSS_AUTHORIZED);
		ui_log_push("Device rejected — contact admin");
		return STATE_ERROR;
	}

	LOG_ERR("Auth check failed (%d) — retry in 10s", rc);
	ui_log_push("Server unreachable — retrying...");
	k_msleep(10000);
	return STATE_WAITING_AUTHORIZATION;
}

static enum app_state do_refresh(void)
{
	if (!last_frame_id[0]) {
		ui_server_status_show(ICON_VPN_KEY,
			"Signing in",
			"Restoring access to your screen service.");
	}

	if (!refresh_token[0]) {
		return STATE_AUTHENTICATING;
	}

	int32_t rc = session_ensure();

	if (rc < 0) {
		k_msleep(5000);
		return STATE_REFRESHING;
	}

	char new_access[LLSS_TOKEN_MAX] = {0};

	rc = llss_refresh_access_token(refresh_token, new_access);
	session_close_on_net_error(rc);

	if (rc == 0) {
		update_access_token(new_access);
		LOG_INF("Token refreshed — polling");
		return STATE_POLLING;
	} else if (rc == -EACCES) {
		LOG_INF("Refresh token rejected — full auth");
		return STATE_AUTHENTICATING;
	}

	LOG_ERR("Token refresh error %d — retry in 5s", rc);
	k_msleep(5000);
	return STATE_REFRESHING;
}

static enum app_state do_authenticate(void)
{
	ui_log_push("Authenticating...");
	ui_server_status_show(ICON_LOCK_CLOCK,
		"Checking access",
		"Confirming this device is ready to receive screens.");

	if (!device_secret[0]) {
		LOG_ERR("No device_secret — re-registering");
		return STATE_REGISTERING;
	}

	int32_t rc = session_ensure();

	if (rc < 0) {
		k_msleep(5000);
		return STATE_AUTHENTICATING;
	}

	enum llss_auth_status auth_status;
	char new_refresh[LLSS_TOKEN_MAX] = {0};

	rc = llss_authenticate(hardware_id, device_secret,
			       &auth_status, new_refresh);

	if (rc == 0 && auth_status == LLSS_AUTH_AUTHORIZED && new_refresh[0]) {
		char new_access[LLSS_TOKEN_MAX] = {0};

		rc = llss_refresh_access_token(new_refresh, new_access);
		session_close_on_net_error(rc);
		if (rc == 0) {
			strncpy(refresh_token, new_refresh,
				sizeof(refresh_token) - 1);
			llss_storage_save_tokens(refresh_token, access_token);
			update_access_token(new_access);
			sys_flag_set(SYS_FLAG_LLSS_AUTHORIZED);
			LOG_INF("Authenticated — polling");
			return STATE_POLLING;
		}
		LOG_ERR("Token exchange failed %d — retry in 5s", rc);
		k_msleep(5000);
		return STATE_AUTHENTICATING;
	} else if (rc == 0 && auth_status == LLSS_AUTH_PENDING) {
		return STATE_WAITING_AUTHORIZATION;
	} else if (auth_status == LLSS_AUTH_REJECTED ||
		   auth_status == LLSS_AUTH_REVOKED || rc == -EACCES) {
		session_close_on_net_error(-EACCES);
		sys_flag_clear(SYS_FLAG_LLSS_AUTHORIZED);
		ui_log_push("Device rejected — contact admin");
		return STATE_ERROR;
	}

	session_close_on_net_error(rc);
	LOG_ERR("Authenticate error %d — retry in 5s", rc);
	k_msleep(5000);
	return STATE_AUTHENTICATING;
}

static enum app_state do_send_input(const struct button_event *bev)
{
	int32_t rc = session_ensure();

	if (rc < 0) {
		k_msleep(5000);
		return STATE_POLLING;
	}

	struct llss_input_response input = {0};

	rc = llss_send_input(access_token, device_id,
			     ui_btn_llss_name(bev->btn),
			     ui_evt_llss_name(bev->evt), &input);
	session_close_on_net_error(rc);

	if (rc == -EACCES) {
		return STATE_REFRESHING;
	}
	if (rc < 0) {
		LOG_ERR("send_input error %d", rc);
		return STATE_POLLING;
	}

	if (input.status == LLSS_INPUT_NEW_FRAME && input.frame_id[0]) {
		strncpy(last_frame_id, input.frame_id,
			sizeof(last_frame_id) - 1);
		return STATE_FETCHING_FRAME;
	} else if (input.status == LLSS_INPUT_POLL && input.poll_after_ms > 0) {
		poll_interval_ms = input.poll_after_ms;
	}
	return STATE_POLLING;
}

static enum app_state do_poll(void)
{
	int32_t rc = session_ensure();

	if (rc < 0) {
		k_msleep(poll_interval_ms);
		return STATE_POLLING;
	}

	struct llss_device_state state = {0};

	rc = llss_get_device_state(access_token, device_id,
				   last_frame_id, &state);
	session_close_on_net_error(rc);

	if (rc == -EACCES) {
		return STATE_REFRESHING;
	}
	if (rc < 0) {
		LOG_ERR("Poll error %d — wait %d ms", rc, poll_interval_ms);
		k_msleep(poll_interval_ms);
		return STATE_POLLING;
	}

	poll_interval_ms = state.poll_after_ms;

	switch (state.action) {
	case LLSS_ACTION_FETCH_FRAME:
		if (state.frame_id[0]) {
			ui_server_status_show(ICON_SYNC,
				"Loading screen",
				"Receiving the latest content from the server.");
		}
		if (state.frame_id[0]) {
			strncpy(last_frame_id, state.frame_id,
				sizeof(last_frame_id) - 1);
			return STATE_FETCHING_FRAME;
		}
		if (!last_frame_id[0]) {
			ui_server_status_show(ICON_SCREEN,
				"Waiting for content",
				"The device is online and waiting for its first screen.");
		}
		k_msleep(poll_interval_ms);
		return STATE_POLLING;
	case LLSS_ACTION_SLEEP:
		if (!last_frame_id[0]) {
			ui_server_status_show(ICON_MONITOR,
				"Waiting for content",
				"The device is online and waiting for its first screen.");
		}
		return STATE_SLEEPING;
	default:
		if (!last_frame_id[0]) {
			ui_server_status_show(ICON_MONITOR,
				"Waiting for content",
				"The device is online and waiting for its first screen.");
		}
		k_msleep(poll_interval_ms);
		return STATE_POLLING;
	}
}

static enum app_state do_fetch_frame(void)
{
	ui_log_push("Fetching frame...");
	ui_server_status_show(ICON_SYNC,
		"Loading screen",
		"Receiving the latest content from the server.");

	int32_t rc = session_ensure();

	if (rc < 0) {
		return STATE_POLLING;
	}

	/* Fetch straight into a display pipeline buffer — no copy. */
	size_t cap = 0;
	uint8_t *dst = display_frame_write_buf(&cap);
	size_t png_len = 0;

	rc = llss_fetch_frame(access_token, device_id, last_frame_id,
			      dst, cap, &png_len);
	session_close_on_net_error(rc);

	if (rc == -EACCES) {
		return STATE_REFRESHING;
	}
	if (rc < 0) {
		LOG_ERR("Fetch frame failed: %d", rc);
		return STATE_POLLING;
	}

	if (png_len > 0) {
		int32_t drc = display_frame_submit(png_len);

		if (drc < 0) {
			LOG_ERR("Frame submit failed: %d", drc);
		}
	}

	return STATE_POLLING;
}

/* =========================================================================
 * Thread entry point
 * ========================================================================= */

static void llss_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	/* Storage init has no network dependency — do it before waiting */
	settings_subsys_init();
	llss_storage_init();
	llss_storage_load(device_id, device_secret, refresh_token, access_token);
	device_ui_settings_ready();
	LOG_INF("Stored device_id: %s", device_id[0] ? device_id : "(none)");

	/* Hardware ID derived from the link-layer MAC.  The MAC is assigned by
	 * the WiFi stack at boot before any association, so we don't need to
	 * wait for SYS_FLAG_WIFI_READY here — only for net_if_get_default(). */
	build_hardware_id();
	LOG_INF("Hardware ID: %s", hardware_id);

	int32_t rc = llss_client_init();

	if (rc) {
		LOG_ERR("llss_client_init: %d", rc);
		ui_log_push("LLSS init failed — check CA cert");
		app_state = STATE_ERROR;
	} else {
		app_state = device_id[0] ? STATE_REFRESHING : STATE_REGISTERING;
	}

	while (true) {
		/* try / fail / wait.  If WiFi is up and clock is valid we
		 * proceed; if either drops, we block here until both return.
		 * No polling, no special "I'm waiting for WiFi" state.
		 *
		 * 30 s timeout is just so a stuck wait surfaces in the serial
		 * log instead of being silent — the wait is still blocking
		 * for as long as needed. */
		uint32_t needed = SYS_FLAG_WIFI_READY | SYS_FLAG_TIME_VALID;
		uint32_t got = sys_flag_wait_all(needed, K_SECONDS(30));

		if (got == 0) {
			uint32_t cur = sys_flag_get();

			LOG_WRN("LLSS stuck waiting: have=0x%08x  "
				"WIFI_READY=%d  TIME_VALID=%d",
				cur,
				!!(cur & SYS_FLAG_WIFI_READY),
				!!(cur & SYS_FLAG_TIME_VALID));
			continue;
		}

		session_check_reset();

		/* Button events are only serviced while polling */
		if (app_state == STATE_POLLING) {
			struct button_event bev;

			if (k_msgq_get(&btn_queue, &bev, K_NO_WAIT) == 0) {
				LOG_INF("Button: %s %s",
					ui_btn_llss_name(bev.btn),
					ui_evt_llss_name(bev.evt));
				app_state = do_send_input(&bev); /* -> POLLING | FETCHING_FRAME | REFRESHING */
				continue;
			}
		}

		switch (app_state) {
		case STATE_REGISTERING:           /* -> REGISTERING | AUTHENTICATING */
			app_state = do_register();
			break;
		case STATE_WAITING_AUTHORIZATION: /* -> WAITING_AUTHORIZATION | REFRESHING | ERROR */
			app_state = do_wait_authorization();
			break;
		case STATE_REFRESHING:            /* -> REFRESHING | AUTHENTICATING | POLLING */
			app_state = do_refresh();
			break;
		case STATE_AUTHENTICATING:        /* -> AUTHENTICATING | REGISTERING | WAITING_AUTHORIZATION | POLLING | ERROR */
			app_state = do_authenticate();
			break;
		case STATE_POLLING:               /* -> POLLING | FETCHING_FRAME | SLEEPING | REFRESHING */
			app_state = do_poll();
			break;
		case STATE_FETCHING_FRAME:        /* -> POLLING | REFRESHING */
			app_state = do_fetch_frame();
			break;
		case STATE_SLEEPING:              /* -> POLLING */
			LOG_INF("Sleeping for %d ms", poll_interval_ms);
			ui_log_push("Sleeping...");
			k_msleep(poll_interval_ms);
			app_state = STATE_POLLING;
			break;
		case STATE_ERROR:                 /* terminal */
			k_msleep(60000);
			break;
		default:
			k_msleep(100);
			break;
		}
	}
}

K_THREAD_DEFINE(llss_thread, LLSS_THREAD_STACK,
		llss_thread_fn, NULL, NULL, NULL,
		LLSS_THREAD_PRIORITY, 0, 0);
