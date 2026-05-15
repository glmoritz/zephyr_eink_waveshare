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
#include <stdio.h>
#include <string.h>

#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/settings/settings.h>

#include "display_thread.h"
#include "llss_client.h"
#include "llss_storage.h"
#include "llss_thread.h"
#include "ntp_sync.h"
#include "wifi_prov.h"

LOG_MODULE_REGISTER(llss, LOG_LEVEL_INF);

#define LLSS_THREAD_STACK    16384
#define LLSS_THREAD_PRIORITY 10

/* =========================================================================
 * State machine
 * ========================================================================= */

enum app_state {
	STATE_BOOTING = 0,
	STATE_WIFI_CONNECTING,
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

static int poll_interval_ms = CONFIG_LLSS_POLL_INTERVAL_MS;

/* =========================================================================
 * Button input
 * ========================================================================= */

struct button_event {
	const char *name;
	const char *type;
};

K_MSGQ_DEFINE(btn_queue, sizeof(struct button_event), 8, 4);

static const char *keycode_to_llss(int code)
{
	switch (code) {
	case INPUT_KEY_ENTER: return "ENTER";
	case INPUT_KEY_ESC:   return "ESC";
	case INPUT_KEY_LEFT:  return "HL_LEFT";
	case INPUT_KEY_RIGHT: return "HL_RIGHT";
	case INPUT_KEY_1:     return "BTN_1";
	case INPUT_KEY_2:     return "BTN_2";
	case INPUT_KEY_3:     return "BTN_3";
	case INPUT_KEY_4:     return "BTN_4";
	case INPUT_KEY_5:     return "BTN_5";
	case INPUT_KEY_6:     return "BTN_6";
	case INPUT_KEY_7:     return "BTN_7";
	case INPUT_KEY_8:     return "BTN_8";
	default:              return NULL;
	}
}

static void on_input(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY || evt->value == 0) {
		return;
	}

	const char *name = keycode_to_llss(evt->code);

	if (!name) {
		return;
	}

	struct button_event bev = {.name = name, .type = "PRESS"};

	k_msgq_put(&btn_queue, &bev, K_NO_WAIT);
}

INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);

/* =========================================================================
 * WiFi / session
 * ========================================================================= */

static K_SEM_DEFINE(sem_wifi_up, 0, 1);
static atomic_t session_reset_requested = ATOMIC_INIT(0);
static bool session_open;

void llss_on_wifi(enum wifi_prov_state state, const char *info)
{
	char msg[80];

	switch (state) {
	case WIFI_PROV_CONNECTING:
		LOG_INF("WiFi connecting: %s", info);
		ui_set_status("Connecting to WiFi...", info);
		break;
	case WIFI_PROV_CONNECTED:
		LOG_INF("WiFi connected: %s", info);
		snprintf(msg, sizeof(msg), "IP: %s", info);
		ui_set_status("WiFi connected", msg);
		k_sem_give(&sem_wifi_up);
		break;
	case WIFI_PROV_DISCONNECTED:
		LOG_WRN("WiFi disconnected");
		ui_set_status("WiFi disconnected", "Reconnecting...");
		atomic_set(&session_reset_requested, 1);
		app_state = STATE_WIFI_CONNECTING;
		break;
	case WIFI_PROV_AP_ACTIVE:
		snprintf(msg, sizeof(msg), "Connect to: %s", info);
		ui_set_status("WiFi Setup — open browser:", msg);
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

	int rc = llss_session_open();

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
	ui_set_status("Connecting to server...", "Registering device...");

	int rc = session_ensure();

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

	LOG_ERR("Registration failed: %d — retry in 10s", rc);
	ui_set_status("Server unreachable", "Retrying in 10s...");
	k_msleep(10000);
	return STATE_REGISTERING;
}

static enum app_state do_wait_authorization(void)
{
	ui_set_status("Waiting for admin", "authorization...");

	int rc = session_ensure();

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
		return STATE_REFRESHING;
	} else if (rc == 0 && auth_status == LLSS_AUTH_PENDING) {
		LOG_INF("Still pending — retry in 15s");
		k_msleep(15000);
		return STATE_WAITING_AUTHORIZATION;
	} else if (auth_status == LLSS_AUTH_REJECTED ||
		   auth_status == LLSS_AUTH_REVOKED || rc == -EACCES) {
		LOG_ERR("Device rejected/revoked");
		ui_set_status("Device rejected", "Contact admin");
		return STATE_ERROR;
	}

	LOG_ERR("Auth check failed (%d) — retry in 10s", rc);
	ui_set_status("Server unreachable", "Retrying...");
	k_msleep(10000);
	return STATE_WAITING_AUTHORIZATION;
}

static enum app_state do_refresh(void)
{
	if (!refresh_token[0]) {
		return STATE_AUTHENTICATING;
	}

	int rc = session_ensure();

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
	ui_set_status("Authenticating...", NULL);

	if (!device_secret[0]) {
		LOG_ERR("No device_secret — re-registering");
		return STATE_REGISTERING;
	}

	int rc = session_ensure();

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
		ui_set_status("Device rejected", "Contact admin");
		return STATE_ERROR;
	}

	session_close_on_net_error(rc);
	LOG_ERR("Authenticate error %d — retry in 5s", rc);
	k_msleep(5000);
	return STATE_AUTHENTICATING;
}

static enum app_state do_send_input(const struct button_event *bev)
{
	int rc = session_ensure();

	if (rc < 0) {
		k_msleep(5000);
		return STATE_POLLING;
	}

	struct llss_input_response input = {0};

	rc = llss_send_input(access_token, device_id,
			     bev->name, bev->type, &input);
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
	int rc = session_ensure();

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
			strncpy(last_frame_id, state.frame_id,
				sizeof(last_frame_id) - 1);
			return STATE_FETCHING_FRAME;
		}
		k_msleep(poll_interval_ms);
		return STATE_POLLING;
	case LLSS_ACTION_SLEEP:
		return STATE_SLEEPING;
	default:
		k_msleep(poll_interval_ms);
		return STATE_POLLING;
	}
}

static enum app_state do_fetch_frame(void)
{
	ui_set_status("Fetching frame...", last_frame_id);

	int rc = session_ensure();

	if (rc < 0) {
		return STATE_POLLING;
	}

	uint8_t *png = NULL;
	size_t png_len = 0;

	rc = llss_fetch_frame(access_token, device_id, last_frame_id,
			      &png, &png_len);
	session_close_on_net_error(rc);

	if (rc == -EACCES) {
		return STATE_REFRESHING;
	}
	if (rc < 0) {
		LOG_ERR("Fetch frame failed: %d", rc);
		return STATE_POLLING;
	}

	if (png && png_len > 0) {
		int drc = queue_display_frame(png, png_len);

		if (drc < 0) {
			LOG_ERR("Queue frame failed: %d", drc);
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
	LOG_INF("Stored device_id: %s", device_id[0] ? device_id : "(none)");

	/* Block until WiFi is up (signalled by llss_on_wifi) */
	k_sem_take(&sem_wifi_up, K_FOREVER);

	build_hardware_id();
	LOG_INF("Hardware ID: %s", hardware_id);

	ntp_sync_restore_from_rtc();
	ntp_sync_start();

	int rc = llss_client_init();

	if (rc) {
		LOG_ERR("llss_client_init: %d", rc);
		ui_set_status("LLSS init failed", "Check CA cert");
		app_state = STATE_ERROR;
	} else {
		app_state = device_id[0] ? STATE_REFRESHING : STATE_REGISTERING;
	}

	while (true) {
		session_check_reset();

		if (app_state == STATE_WIFI_CONNECTING) {
			k_msleep(100);
			continue;
		}

		/* Button events are only serviced while polling */
		if (app_state == STATE_POLLING) {
			struct button_event bev;

			if (k_msgq_get(&btn_queue, &bev, K_NO_WAIT) == 0) {
				LOG_INF("Button: %s %s", bev.name, bev.type);
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
			ui_set_status("Sleeping...", NULL);
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
