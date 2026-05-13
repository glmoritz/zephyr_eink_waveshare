/*
 * LLSS e-Ink Client — main state machine
 *
 * Board: eink_llss_esp32 (ESP32-S3 N16R8)
 * RTOS:  Zephyr 4.3.0
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/settings/settings.h>

#include <lvgl.h>

#include "llss_client.h"
#include "llss_storage.h"
#include "ntp_sync.h"
#include "wifi_prov.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* =========================================================================
 * Device nodes
 * ========================================================================= */

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)

/* =========================================================================
 * State machine
 * ========================================================================= */

enum app_state {
	STATE_BOOTING = 0,
	STATE_WIFI_CONNECTING,
	STATE_WIFI_CONNECTED,
	STATE_REGISTERING,
	STATE_WAITING_AUTHORIZATION,
	STATE_AUTHENTICATING,
	STATE_POLLING,
	STATE_FETCHING_FRAME,
	STATE_DISPLAYING,
	STATE_SENDING_INPUT,
	STATE_SLEEPING,
	STATE_ERROR,
};

static volatile enum app_state app_state = STATE_BOOTING;

/* =========================================================================
 * Shared credentials (main thread only)
 * ========================================================================= */

static char device_id[LLSS_DEVICE_ID_MAX];
static char device_secret[LLSS_DEVICE_SECRET_MAX];
static char refresh_token[LLSS_TOKEN_MAX];
static char access_token[LLSS_TOKEN_MAX];
static char hardware_id[16];
static char last_frame_id[64];

static int poll_interval_ms = CONFIG_LLSS_POLL_INTERVAL_MS;

/* =========================================================================
 * Button input queue
 * ========================================================================= */

struct button_event {
	const char *name;
	const char *type;
};

/* =========================================================================
 * Display worker
 * ========================================================================= */

#define DISPLAY_THREAD_STACK_SIZE 12288
#define DISPLAY_THREAD_PRIORITY   12
#define LLSS_API_THREAD_STACK_SIZE 16384
#define LLSS_API_THREAD_PRIORITY   10

K_MUTEX_DEFINE(lvgl_mutex);
K_SEM_DEFINE(display_work_sem, 0, 1);
K_SEM_DEFINE(display_slot_sem, 1, 1);

static uint8_t display_png_buf[CONFIG_LLSS_FRAME_BUF_SIZE]
	__attribute__((section(".ext_ram_noinit.llss_display")));
static size_t display_png_len;
static bool display_worker_ready;

K_THREAD_STACK_DEFINE(display_thread_stack, DISPLAY_THREAD_STACK_SIZE);
static struct k_thread display_thread_data;

enum llss_job_type {
	LLSS_JOB_NONE = 0,
	LLSS_JOB_REGISTER,
	LLSS_JOB_WAIT_AUTHORIZATION,
	LLSS_JOB_AUTHENTICATE,
	LLSS_JOB_POLL,
	LLSS_JOB_FETCH_FRAME,
	LLSS_JOB_SEND_INPUT,
};

struct llss_job_request {
	enum llss_job_type type;
	char hardware_id[sizeof(hardware_id)];
	char device_id[LLSS_DEVICE_ID_MAX];
	char device_secret[LLSS_DEVICE_SECRET_MAX];
	char refresh_token[LLSS_TOKEN_MAX];
	char access_token[LLSS_TOKEN_MAX];
	char last_frame_id[sizeof(last_frame_id)];
	struct button_event button;
};

struct llss_job_result {
	enum llss_job_type type;
	int rc;
	char new_access_token[LLSS_TOKEN_MAX];
	union {
		struct {
			char device_id[LLSS_DEVICE_ID_MAX];
			char device_secret[LLSS_DEVICE_SECRET_MAX];
		} registration;
		struct {
			enum llss_auth_status auth_status;
			char refresh_token[LLSS_TOKEN_MAX];
		} auth;
		struct llss_device_state state;
		struct {
			size_t png_len;
			int display_rc;
		} frame;
		struct llss_input_response input;
	} data;
};

K_MSGQ_DEFINE(llss_job_queue, sizeof(struct llss_job_request), 1, 4);
K_MSGQ_DEFINE(llss_result_queue, sizeof(struct llss_job_result), 1, 4);

K_THREAD_STACK_DEFINE(llss_api_thread_stack, LLSS_API_THREAD_STACK_SIZE);
static struct k_thread llss_api_thread_data;
static bool llss_job_in_flight;
static struct button_event pending_button_event;

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
 * LVGL status display
 * ========================================================================= */

static lv_obj_t *status_label;
static lv_obj_t *sub_label;

static void ui_refresh_locked(void)
{
	lv_task_handler();

	const struct device *disp = DEVICE_DT_GET(DISPLAY_NODE);

	if (device_is_ready(disp)) {
		display_blanking_off(disp);
	}
}

static void ui_init(void)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);

	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

	status_label = lv_label_create(scr);
	lv_label_set_text(status_label, "Booting...");
	lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
	lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -20);

	sub_label = lv_label_create(scr);
	lv_label_set_text(sub_label, "");
	lv_obj_set_style_text_font(sub_label, &lv_font_montserrat_14, 0);
	lv_obj_align(sub_label, LV_ALIGN_CENTER, 0, 20);

	ui_refresh_locked();
	k_mutex_unlock(&lvgl_mutex);
}

static void ui_set_status(const char *line1, const char *line2)
{
	k_mutex_lock(&lvgl_mutex, K_FOREVER);

	if (status_label) {
		lv_label_set_text(status_label, line1 ? line1 : "");
	}
	if (sub_label) {
		lv_label_set_text(sub_label, line2 ? line2 : "");
	}
	ui_refresh_locked();
	k_mutex_unlock(&lvgl_mutex);
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
 * WiFi callbacks
 * ========================================================================= */

static K_SEM_DEFINE(sem_wifi_up, 0, 1);

static void on_wifi(enum wifi_prov_state state, const char *info)
{
	/* "IP: <ipv4> / <ipv6>" needs up to 4 + 15 + 3 + 39 = 61 chars */
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

/* =========================================================================
 * PNG frame display via LVGL lodepng
 * ========================================================================= */

static lv_obj_t *frame_img;
static lv_image_dsc_t frame_dsc;

static void display_png_frame_locked(const uint8_t *png_buf, size_t png_len)
{
	memset(&frame_dsc, 0, sizeof(frame_dsc));
	frame_dsc.header.cf  = LV_COLOR_FORMAT_L8;
	frame_dsc.header.w   = 800;
	frame_dsc.header.h   = 480;
	frame_dsc.data_size  = png_len;
	frame_dsc.data       = png_buf;

	if (!frame_img) {
		lv_obj_t *scr = lv_screen_active();

		frame_img = lv_image_create(scr);
		lv_obj_align(frame_img, LV_ALIGN_CENTER, 0, 0);
	}

	lv_image_set_src(frame_img, &frame_dsc);
	ui_refresh_locked();
}

static void display_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		k_sem_take(&display_work_sem, K_FOREVER);

		LOG_INF("Displaying frame (%zu bytes)", display_png_len);

		k_mutex_lock(&lvgl_mutex, K_FOREVER);
		display_png_frame_locked(display_png_buf, display_png_len);
		k_mutex_unlock(&lvgl_mutex);

		k_sem_give(&display_slot_sem);
	}
}

static int queue_display_frame(const uint8_t *png_buf, size_t png_len)
{
	if (!display_worker_ready) {
		return -ENODEV;
	}

	if (png_len == 0 || png_len > sizeof(display_png_buf)) {
		return -EMSGSIZE;
	}

	k_sem_take(&display_slot_sem, K_FOREVER);
	memcpy(display_png_buf, png_buf, png_len);
	display_png_len = png_len;
	k_sem_give(&display_work_sem);

	return 0;
}

static void llss_api_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		struct llss_job_request req;
		struct llss_job_result result = {0};

		k_msgq_get(&llss_job_queue, &req, K_FOREVER);
		result.type = req.type;

		/* Phase A: open one TLS session shared by all llss_* calls in
		 * this job.  A single TLS handshake covers the entire wake cycle
		 * regardless of how many API endpoints are visited. */
		result.rc = llss_session_open();
		if (result.rc < 0) {
			LOG_ERR("llss_session_open: %d — skipping job", result.rc);
			k_msgq_put(&llss_result_queue, &result, K_FOREVER);
			continue;
		}

		switch (req.type) {
		case LLSS_JOB_REGISTER:
			result.rc = llss_register(req.hardware_id,
						  result.data.registration.device_id,
						  result.data.registration.device_secret);
			break;
		case LLSS_JOB_WAIT_AUTHORIZATION:
			result.rc = llss_authenticate(req.hardware_id,
						      req.device_secret,
						      &result.data.auth.auth_status,
						      result.data.auth.refresh_token);
			break;
		case LLSS_JOB_AUTHENTICATE:
			result.data.auth.auth_status = LLSS_AUTH_UNKNOWN;

			if (req.refresh_token[0]) {
				result.rc = llss_refresh_access_token(req.refresh_token,
							     result.new_access_token);
				if (result.rc == 0) {
					result.data.auth.auth_status = LLSS_AUTH_AUTHORIZED;
					break;
				}
				if (result.rc != -EACCES) {
					break;
				}
			}

			if (!req.device_secret[0]) {
				result.rc = -ENOENT;
				break;
			}

			result.rc = llss_authenticate(req.hardware_id,
						      req.device_secret,
						      &result.data.auth.auth_status,
						      result.data.auth.refresh_token);
			if (result.rc == 0 &&
			    result.data.auth.auth_status == LLSS_AUTH_AUTHORIZED &&
			    result.data.auth.refresh_token[0]) {
				result.rc = llss_refresh_access_token(
					result.data.auth.refresh_token,
					result.new_access_token);
			}
			break;
		case LLSS_JOB_POLL:
			result.rc = llss_get_device_state(req.access_token,
						 req.device_id,
						 req.last_frame_id,
						 &result.data.state);
			if (result.rc == -EACCES && req.refresh_token[0]) {
				result.rc = llss_refresh_access_token(req.refresh_token,
							     result.new_access_token);
				if (result.rc == 0) {
					result.rc = llss_get_device_state(
						result.new_access_token,
						req.device_id,
						req.last_frame_id,
						&result.data.state);
				}
			}
			break;
		case LLSS_JOB_FETCH_FRAME:
		{
			uint8_t *png = NULL;
			size_t png_len = 0;

			result.rc = llss_fetch_frame(req.access_token,
						     req.device_id,
						     req.last_frame_id,
						     &png, &png_len);
			if (result.rc == -EACCES && req.refresh_token[0]) {
				result.rc = llss_refresh_access_token(req.refresh_token,
							     result.new_access_token);
				if (result.rc == 0) {
					result.rc = llss_fetch_frame(result.new_access_token,
							     req.device_id,
							     req.last_frame_id,
							     &png, &png_len);
				}
			}

			if (result.rc == 0 && png && png_len > 0) {
				result.data.frame.png_len = png_len;
				result.data.frame.display_rc = queue_display_frame(png, png_len);
			}
			break;
		}
		case LLSS_JOB_SEND_INPUT:
			result.rc = llss_send_input(req.access_token,
						    req.device_id,
						    req.button.name,
						    req.button.type,
						    &result.data.input);
			if (result.rc == -EACCES && req.refresh_token[0]) {
				result.rc = llss_refresh_access_token(req.refresh_token,
							     result.new_access_token);
				if (result.rc == 0) {
					result.rc = llss_send_input(result.new_access_token,
							    req.device_id,
							    req.button.name,
							    req.button.type,
							    &result.data.input);
				}
			}
			break;
		default:
			result.rc = -EINVAL;
			break;
		}

		llss_session_close();
		k_msgq_put(&llss_result_queue, &result, K_FOREVER);
	}
}

static int submit_llss_job(enum llss_job_type type,
				   const struct button_event *button)
{
	if (llss_job_in_flight) {
		return -EALREADY;
	}

	struct llss_job_request req = {
		.type = type,
	};

	strncpy(req.hardware_id, hardware_id, sizeof(req.hardware_id) - 1);
	strncpy(req.device_id, device_id, sizeof(req.device_id) - 1);
	strncpy(req.device_secret, device_secret, sizeof(req.device_secret) - 1);
	strncpy(req.refresh_token, refresh_token, sizeof(req.refresh_token) - 1);
	strncpy(req.access_token, access_token, sizeof(req.access_token) - 1);
	strncpy(req.last_frame_id, last_frame_id, sizeof(req.last_frame_id) - 1);

	if (button) {
		req.button = *button;
	}

	int rc = k_msgq_put(&llss_job_queue, &req, K_NO_WAIT);

	if (rc == 0) {
		llss_job_in_flight = true;
	}

	return rc;
}

static void update_access_token(const char *token)
{
	if (!token[0]) {
		return;
	}

	strncpy(access_token, token, sizeof(access_token) - 1);
	llss_storage_save_access_token(access_token);
}

static void handle_llss_job_result(const struct llss_job_result *result)
{
	update_access_token(result->new_access_token);

	switch (result->type) {
	case LLSS_JOB_REGISTER:
		if (result->rc == 0) {
			strncpy(device_id, result->data.registration.device_id,
				sizeof(device_id) - 1);
			strncpy(device_secret, result->data.registration.device_secret,
				sizeof(device_secret) - 1);
			llss_storage_save_device(device_id, device_secret);
			LOG_INF("Registered: %s", device_id);
			app_state = STATE_AUTHENTICATING;
		} else {
			LOG_ERR("Registration failed: %d — retry in 10s", result->rc);
			ui_set_status("Server unreachable", "Retrying in 10s...");
			k_msleep(10000);
		}
		break;
	case LLSS_JOB_WAIT_AUTHORIZATION:
		if (result->rc == 0 &&
		    result->data.auth.auth_status == LLSS_AUTH_AUTHORIZED &&
		    result->data.auth.refresh_token[0]) {
			strncpy(refresh_token, result->data.auth.refresh_token,
				sizeof(refresh_token) - 1);
			llss_storage_save_tokens(refresh_token, "");
			LOG_INF("Authorized — got refresh token");
			app_state = STATE_AUTHENTICATING;
		} else if (result->rc == 0 &&
			   result->data.auth.auth_status == LLSS_AUTH_PENDING) {
			LOG_INF("Still pending — retry in 15s");
			k_msleep(15000);
		} else if (result->data.auth.auth_status == LLSS_AUTH_REJECTED ||
			   result->data.auth.auth_status == LLSS_AUTH_REVOKED ||
			   result->rc == -EACCES) {
			LOG_ERR("Device rejected/revoked");
			ui_set_status("Device rejected", "Contact admin");
			app_state = STATE_ERROR;
		} else {
			LOG_ERR("Auth check failed (%d) — retry in 10s", result->rc);
			ui_set_status("Server unreachable", "Retrying...");
			k_msleep(10000);
		}
		break;
	case LLSS_JOB_AUTHENTICATE:
		if (result->rc == 0 && result->new_access_token[0]) {
			if (result->data.auth.refresh_token[0]) {
				strncpy(refresh_token, result->data.auth.refresh_token,
					sizeof(refresh_token) - 1);
				llss_storage_save_tokens(refresh_token, access_token);
			} else {
				llss_storage_save_access_token(access_token);
			}
			LOG_INF("Access token obtained — polling");
			app_state = STATE_POLLING;
		} else if (result->rc == 0 &&
			   result->data.auth.auth_status == LLSS_AUTH_PENDING) {
			app_state = STATE_WAITING_AUTHORIZATION;
		} else if (result->rc == -ENOENT) {
			LOG_ERR("No device_secret — re-registering");
			app_state = STATE_REGISTERING;
		} else if (result->data.auth.auth_status == LLSS_AUTH_REJECTED ||
			   result->data.auth.auth_status == LLSS_AUTH_REVOKED ||
			   result->rc == -EACCES) {
			ui_set_status("Device rejected", "Contact admin");
			app_state = STATE_ERROR;
		} else {
			LOG_ERR("Authenticate error %d — retry in 5s", result->rc);
			k_msleep(5000);
		}
		break;
	case LLSS_JOB_POLL:
		if (result->rc == -EACCES) {
			app_state = STATE_AUTHENTICATING;
			break;
		}

		if (result->rc < 0) {
			LOG_ERR("Poll error %d — wait %d ms", result->rc,
				poll_interval_ms);
			k_msleep(poll_interval_ms);
			break;
		}

		poll_interval_ms = result->data.state.poll_after_ms;

		switch (result->data.state.action) {
		case LLSS_ACTION_FETCH_FRAME:
			if (result->data.state.frame_id[0]) {
				strncpy(last_frame_id, result->data.state.frame_id,
					sizeof(last_frame_id) - 1);
				app_state = STATE_FETCHING_FRAME;
			}
			break;
		case LLSS_ACTION_SLEEP:
			app_state = STATE_SLEEPING;
			break;
		default:
			k_msleep(poll_interval_ms);
			break;
		}
		break;
	case LLSS_JOB_FETCH_FRAME:
		if (result->rc == -EACCES) {
			app_state = STATE_AUTHENTICATING;
			break;
		}

		if (result->rc < 0) {
			LOG_ERR("Fetch frame failed: %d", result->rc);
			app_state = STATE_POLLING;
			break;
		}

		if (result->data.frame.display_rc < 0) {
			LOG_ERR("Queue frame failed: %d", result->data.frame.display_rc);
		}

		app_state = STATE_POLLING;
		break;
	case LLSS_JOB_SEND_INPUT:
		if (result->rc == -EACCES) {
			app_state = STATE_AUTHENTICATING;
			break;
		}

		if (result->rc < 0) {
			LOG_ERR("send_input error %d", result->rc);
			app_state = STATE_POLLING;
			break;
		}

		if (result->data.input.status == LLSS_INPUT_NEW_FRAME &&
		    result->data.input.frame_id[0]) {
			strncpy(last_frame_id, result->data.input.frame_id,
				sizeof(last_frame_id) - 1);
			app_state = STATE_FETCHING_FRAME;
		} else if (result->data.input.status == LLSS_INPUT_POLL &&
			   result->data.input.poll_after_ms > 0) {
			poll_interval_ms = result->data.input.poll_after_ms;
			app_state = STATE_POLLING;
		} else {
			app_state = STATE_POLLING;
		}
		break;
	default:
		break;
	}
}

/* =========================================================================
 * Auth helpers
 * ========================================================================= */

/* =========================================================================
 * State machine handlers
 * ========================================================================= */

static void handle_registering(void)
{
	if (llss_job_in_flight) {
		return;
	}

	ui_set_status("Connecting to server...", "Registering device...");
	submit_llss_job(LLSS_JOB_REGISTER, NULL);
}

static void handle_waiting_authorization(void)
{
	if (llss_job_in_flight) {
		return;
	}

	ui_set_status("Waiting for admin", "authorization...");
	submit_llss_job(LLSS_JOB_WAIT_AUTHORIZATION, NULL);
}

static void handle_authenticating(void)
{
	if (llss_job_in_flight) {
		return;
	}

	ui_set_status("Authenticating...", NULL);

	if (!device_secret[0] && !refresh_token[0]) {
		LOG_ERR("No device_secret — re-registering");
		app_state = STATE_REGISTERING;
		return;
	}

	submit_llss_job(LLSS_JOB_AUTHENTICATE, NULL);
}

static void handle_polling(void)
{
	if (llss_job_in_flight) {
		return;
	}

	submit_llss_job(LLSS_JOB_POLL, NULL);
}

static void handle_fetching_frame(void)
{
	if (llss_job_in_flight) {
		return;
	}

	ui_set_status("Fetching frame...", last_frame_id);
	app_state = STATE_DISPLAYING;
	submit_llss_job(LLSS_JOB_FETCH_FRAME, NULL);
}

static void handle_sending_input(void)
{
	if (llss_job_in_flight) {
		return;
	}

	submit_llss_job(LLSS_JOB_SEND_INPUT, &pending_button_event);
}

/* =========================================================================
 * main()
 * ========================================================================= */

int main(void)
{
	k_msleep(1500); /* USB JTAG enumeration */

	LOG_INF("=== LLSS e-Ink Client v%s ===", CONFIG_LLSS_FIRMWARE_VERSION);

	/* ---- Display + LVGL ------------------------------------------------ */
	const struct device *disp = DEVICE_DT_GET(DISPLAY_NODE);

	if (!device_is_ready(disp)) {
		LOG_ERR("Display not ready!");
	} else {
		lv_lodepng_init();
		ui_init();
		k_thread_create(&display_thread_data, display_thread_stack,
				K_THREAD_STACK_SIZEOF(display_thread_stack),
				display_thread_fn,
				NULL, NULL, NULL,
				DISPLAY_THREAD_PRIORITY, 0, K_NO_WAIT);
		k_thread_name_set(&display_thread_data, "llss_display");
		display_worker_ready = true;
	}

	k_thread_create(&llss_api_thread_data, llss_api_thread_stack,
			K_THREAD_STACK_SIZEOF(llss_api_thread_stack),
			llss_api_thread_fn,
			NULL, NULL, NULL,
			LLSS_API_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&llss_api_thread_data, "llss_api");

	/* ---- Settings (must precede llss_storage_init) --------------------- */
	settings_subsys_init();

	/* ---- Persistent storage -------------------------------------------- */
	llss_storage_init();
	llss_storage_load(device_id, device_secret, refresh_token, access_token);

	LOG_INF("Stored device_id: %s",
		device_id[0] ? device_id : "(none)");

	/* ---- WiFi ---------------------------------------------------------- */
	wifi_prov_init(on_wifi);
	ui_set_status("Connecting to WiFi...", NULL);
	app_state = STATE_WIFI_CONNECTING;
	wifi_prov_start(); /* blocks until connected */

	k_sem_take(&sem_wifi_up, K_FOREVER);

	/* ---- Time: restore from RTC, then start hourly NTP sync ----------- */
	ntp_sync_restore_from_rtc();
	ntp_sync_start();

	/* ---- Hardware ID --------------------------------------------------- */
	build_hardware_id();
	LOG_INF("Hardware ID: %s", hardware_id);

	/* ---- LLSS HTTP client ---------------------------------------------- */
	int rc = llss_client_init();

	if (rc) {
		LOG_ERR("llss_client_init: %d", rc);
		ui_set_status("LLSS init failed", "Check CA cert");
		app_state = STATE_ERROR;
	} else {
		app_state = device_id[0] ? STATE_AUTHENTICATING
					 : STATE_REGISTERING;
	}

	/* ---- Main event loop ----------------------------------------------- */
	while (true) {
		struct button_event bev;
		struct llss_job_result result;

		if (k_msgq_get(&llss_result_queue, &result, K_NO_WAIT) == 0) {
			llss_job_in_flight = false;
			handle_llss_job_result(&result);
			continue;
		}

		if (app_state == STATE_POLLING &&
		    !llss_job_in_flight &&
		    k_msgq_get(&btn_queue, &bev, K_NO_WAIT) == 0) {
			LOG_INF("Button: %s %s", bev.name, bev.type);
			pending_button_event = bev;
			app_state = STATE_SENDING_INPUT;
			handle_sending_input();
			continue;
		}

		switch (app_state) {
		case STATE_REGISTERING:
			handle_registering();
			break;
		case STATE_WAITING_AUTHORIZATION:
			handle_waiting_authorization();
			break;
		case STATE_AUTHENTICATING:
			handle_authenticating();
			break;
		case STATE_POLLING:
			handle_polling();
			break;
		case STATE_FETCHING_FRAME:
			handle_fetching_frame();
			break;
		case STATE_SLEEPING:
			LOG_INF("Sleeping for %d ms", poll_interval_ms);
			ui_set_status("Sleeping...", NULL);
			k_msleep(poll_interval_ms);
			app_state = STATE_POLLING;
			break;
		case STATE_ERROR:
			k_msleep(60000);
			break;
		default:
			k_msleep(100);
			break;
		}
		k_msleep(llss_job_in_flight ? 10 : 1);
	}

	return 0;
}