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
#include "llss_strip_cache.h"
#include "llss_thread.h"
#include "press_feedback.h"
#include "section_attrs.h"
#include "material_icons.h"
#include "net_watchdog.h"
#include "sys_watchdog.h"
#include "system_flags.h"
#include "wifi_prov.h"

#if defined(CONFIG_LLSS_PATTERN_TEST)
#include "pattern_check.h"
#endif

LOG_MODULE_REGISTER(llss, LOG_LEVEL_INF);

#define LLSS_THREAD_STACK    16384
#define LLSS_THREAD_PRIORITY 10

/* Consecutive HTTP 401/403 (-EACCES) full-auth failures tolerated before we
 * assume the server has dropped this device's record and self-heal by wiping
 * the stale credentials and re-registering. Guards against a transient 401
 * nuking otherwise-valid credentials. */
#define LLSS_CRED_REJECT_RESET_THRESHOLD 3

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

/* Latest pressed-strip ids advertised by the server. Updated on every state
 * or input response; consumed by prefetch_pending_strips() which fills the
 * SPIRAM strip cache so the press handler can blit feedback without an HTTP
 * round-trip. Empty string = no strip for that band. */
static char latest_top_strip_id[LLSS_STRIP_ID_MAX];
static char latest_bottom_strip_id[LLSS_STRIP_ID_MAX];

/* Latest enabled-slot bitmasks. -1 = server didn't advertise one this cycle
 * (press_feedback falls back to its capture-time heuristic). */
static int32_t latest_top_enabled_mask    = -1;
static int32_t latest_bottom_enabled_mask = -1;

/* Scratch buffer for a single strip fetch. PSRAM-backed; sized for the
 * larger of the two bands. */
/* +1 byte covers the NUL terminator do_request reserves at the end of
 * its receive buffer (room = buf_size - len - 1). Without it the strip
 * body — which is exactly width × height / 8 — gets truncated by one
 * byte and triggers "HTTP response truncated". */
#define STRIP_FETCH_BYTES (800 * MAX(CONFIG_LLSS_TOP_STRIP_HEIGHT, \
				     CONFIG_LLSS_BOTTOM_STRIP_HEIGHT) / 8 + 1)
static uint8_t strip_fetch_buf[STRIP_FETCH_BYTES]
	LLSS_EXT_RAM_NOINIT("strip_fetch");

/* Count of consecutive full-auth credential rejections (see self-heal in
 * do_full_auth). Reset to 0 on any successful authentication. */
static int auth_reject_streak;

/* Count of consecutive server-reachability failures while Wi-Fi is up (see
 * note_server_failure). Reset to 0 on any successful server exchange. */
static int net_fail_streak;

/* Liveness-watchdog handle for this thread (CONFIG_LLSS_HW_WATCHDOG); -1 when
 * disabled. Marked idle around the long blocking waits so an intentional poll/
 * sleep wait is never mistaken for a hang. */
static int llss_wdt = -1;

static int32_t poll_interval_ms = CONFIG_LLSS_POLL_INTERVAL_MS;

/* Server-side hint for the next frame fetch. Captured from /state or
 * /inputs when the action is FETCH_FRAME/NEW_FRAME; consumed by
 * do_fetch_frame when it calls display_frame_submit. */
static bool latest_full_refresh;

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
	uint32_t                  press_t_ms;
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

	LOG_INF("BTNTRACE enqueue code=%d evt=%d t=%u",
		(int)code, (int)evt, k_uptime_get_32());

	k_msgq_put(&btn_queue, &bev, K_NO_WAIT);
}

static void long_press_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct key_slot *slot = CONTAINER_OF(dwork, struct key_slot, long_work);
	uint32_t now = k_uptime_get_32();

	/* CAS guards against a release that beat us to the IDLE transition. */
	LOG_INF("BTNTRACE longfire code=%d dt=%u t=%u",
		(int)slot->code, now - slot->press_t_ms, now);
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
		slot->press_t_ms = k_uptime_get_32();
		LOG_INF("BTNTRACE input press code=%d t=%u",
			(int)evt->code, slot->press_t_ms);
		/* Press down — arm the long-press timer.  reschedule (vs
		 * schedule) handles a stray re-press without an intervening
		 * release by restarting the count. */
		atomic_set(&slot->state, KEY_PRESSED);
		k_work_reschedule(&slot->long_work, K_MSEC(LONG_PRESS_MS));

		/* Immediate e-ink press feedback. This is a pure latch + sem give:
		 * it returns at once and the slow panel blit runs on the display
		 * thread (priority 12, below LLSS at 10 and input at 11), so it can
		 * neither delay this callback nor preempt the network path.
		 *
		 * Starting here — rather than on release — is what makes the blit
		 * overlap the input HTTP round-trip instead of following it: the
		 * panel is already painting while do_send_input() is on the wire,
		 * and the two take roughly the same time. The display loop drops a
		 * stale feedback request if a real server frame landed meanwhile,
		 * so the frame always wins the panel. */
		ui_press_feedback_request(ui_btn_from_keycode(evt->code));
		return;
	}

	/* Release.  Cancel a still-pending long-press work; whoever wins the
	 * CAS owns the event.  If we win PRESSED→IDLE the work hadn't fired
	 * yet — emit PRESS.  Otherwise it already fired LONG_PRESS and we
	 * drop the release silently. */
	uint32_t release_t_ms = k_uptime_get_32();
	LOG_INF("BTNTRACE input release code=%d dt=%u t=%u",
		(int)evt->code, release_t_ms - slot->press_t_ms, release_t_ms);
	k_work_cancel_delayable(&slot->long_work);

	if (atomic_cas(&slot->state, KEY_PRESSED, KEY_IDLE)) {
		enqueue_event(slot->code, UI_EVT_PRESS);
	} else {
		atomic_set(&slot->state, KEY_IDLE);
	}
}

INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);

/* =========================================================================
 * Shell — inject semantic button events
 *
 * The shell command already knows whether the operator requested PRESS or
 * LONG_PRESS, so it should not route through the physical press timing state
 * machine. That path is intentionally tied to real input timing, display-side
 * feedback, and long-press arbitration; using it for shell taps makes the
 * synthetic command hostage to unrelated scheduler/panel latency.
 *
 * Instead, the shell path requests the same immediate press-feedback overlay,
 * waits the nominal press duration, then enqueues the requested semantic
 * event directly. This preserves console usefulness for protocol testing while
 * leaving the real hardware path unchanged.
 * ========================================================================= */

static int shell_btn(const struct shell *sh, size_t argc, char **argv,
		     bool long_press)
{
	if (argc < 2) {
		shell_error(sh, "Usage: btn %s <NAME>", long_press ? "long" : "press");
		shell_print(sh, "  Bottom strip : BTN_1..BTN_8");
		shell_print(sh, "  Top strip    : BTN_10 (menu) | BTN_13 (HL_LEFT) |");
		shell_print(sh, "                 BTN_14 (HL_RIGHT) | BTN_15 (ENTER) |");
		shell_print(sh, "                 BTN_16 (ESC)");
		shell_print(sh, "  Aliases      : ENTER, ESC, HL_LEFT, HL_RIGHT, MENU");
		shell_print(sh, "  Unmapped     : BTN_9 (no hardware), BTN_11..BTN_12");
		return -EINVAL;
	}

	int32_t code = ui_btn_to_keycode(ui_btn_from_name(argv[1]));

	if (code < 0) {
		shell_error(sh, "Unknown button: %s "
				"(try 1..8, 10, 13..16, or their aliases)",
				argv[1]);
		return -EINVAL;
	}

	uint32_t now = k_uptime_get_32();

	LOG_INF("BTNTRACE shell press_send code=%d t=%u rc=0",
		(int)code, now);

	/* Same cosmetic latch the physical key-down does, so shell taps exercise
	 * the real feedback/HTTP overlap. Non-blocking, so the nominal press
	 * duration below stays honest. */
	ui_press_feedback_request(ui_btn_from_keycode(code));

	k_msleep(long_press ? (LONG_PRESS_MS + 100) : 50);

	LOG_INF("BTNTRACE shell release_send code=%d t=%u",
		(int)code, k_uptime_get_32());
	enqueue_event(code, long_press ? UI_EVT_LONG_PRESS : UI_EVT_PRESS);
	LOG_INF("BTNTRACE shell release_done code=%d t=%u rc=0",
		(int)code, k_uptime_get_32());

	LOG_INF("BTNTRACE shell injected kind=%s name=%s code=%d t=%u",
		long_press ? "LONG_PRESS" : "PRESS", argv[1], (int)code,
		k_uptime_get_32());
	return 0;
}

static int cmd_btn_shortcut(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: b <1..8|10|13..16|en|esc|hl|hr|menu>");
		return -EINVAL;
	}

	return shell_btn(sh, argc, argv, false);
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
		      "Short press: btn press <BTN_1..8|BTN_10|BTN_13..16|"
		      "ENTER|ESC|HL_LEFT|HL_RIGHT|MENU>",
		      cmd_btn_press, 2, 0),
	SHELL_CMD_ARG(long,  NULL,
		      "Long press:  btn long  <BTN_1..8|BTN_10|BTN_13..16|"
		      "ENTER|ESC|HL_LEFT|HL_RIGHT|MENU>",
		      cmd_btn_long, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(btn, &sub_btn,
		   "Inject button events through the input subsystem", NULL);

SHELL_CMD_ARG_REGISTER(b, NULL,
		       "Shortcut for short button press: "
		       "b <1..8|10|13..16|en|esc|hl|hr|menu>",
		       cmd_btn_shortcut, 2, 0);

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
		snprintf(msg, sizeof(msg), "Wi-Fi conectando: %s", info);
		ui_log_push(msg);
		ui_server_status_show(ICON_WIFI,
			"Conectando ao Wi-Fi",
			(info && info[0]) ? info : "Entrando na rede salva.");
		sys_flag_clear(SYS_FLAG_WIFI_READY | SYS_FLAG_WIFI_PROVISIONING |
			       SYS_FLAG_SERVER_ONLINE);
		break;
	case WIFI_PROV_CONNECTED:
		LOG_INF("WiFi connected: %s", info);
		snprintf(msg, sizeof(msg), "Wi-Fi conectado, IP: %s", info);
		ui_log_push(msg);
		if (!last_frame_id[0]) {
			ui_server_status_show(ICON_SCHEDULE,
				"Conectado",
				"Sincronizando horario e preparando sua primeira tela.");
		}
		sys_flag_clear(SYS_FLAG_WIFI_PROVISIONING);
		sys_flag_set(SYS_FLAG_WIFI_READY);
		break;
	case WIFI_PROV_DISCONNECTED:
		LOG_WRN("WiFi disconnected");
		ui_log_push("WiFi desconectado, reconectando...");
		if (!last_frame_id[0]) {
			ui_server_status_show(ICON_WARNING,
				"Conexao perdida",
				"Tentando reconectar ao Wi-Fi.");
		}
		sys_flag_clear(SYS_FLAG_WIFI_READY | SYS_FLAG_WIFI_PROVISIONING |
			       SYS_FLAG_SERVER_ONLINE);
		/* Tear down the held TLS session — the underlying TCP is dead.
		 * The next iteration will block on SYS_FLAG_WIFI_READY at the
		 * top of the loop until the link comes back. */
		atomic_set(&session_reset_requested, 1);
		break;
	case WIFI_PROV_AP_ACTIVE:
		snprintf(msg, sizeof(msg), "Modo AP, conecte-se a: %s", info);
		ui_log_push(msg);
		ui_server_status_show(ICON_WIFI_HOTSPOT,
			"Configurar Wi-Fi",
			(info && info[0]) ? info : "Conecte-se ao ponto de acesso do dispositivo para continuar.");
		sys_flag_clear(SYS_FLAG_WIFI_READY | SYS_FLAG_SERVER_ONLINE);
		sys_flag_set(SYS_FLAG_WIFI_PROVISIONING);
		break;
	default:
		break;
	}
}

static void session_check_reset(void)
{
	if (atomic_cas(&session_reset_requested, 1, 0)) {
		llss_session_close();
		session_open = false;
	}
}

static int session_ensure(void)
{
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

/* Drop all locally-held credentials (NVS + in-memory) so the next boot/loop
 * re-registers from scratch. Used by the self-heal path when the server no
 * longer recognises this device's credentials. */
static void wipe_local_credentials(void)
{
	llss_storage_clear();
	device_id[0]     = '\0';
	device_secret[0] = '\0';
	refresh_token[0] = '\0';
	access_token[0]  = '\0';
	auth_reject_streak = 0;
}

/* Record a successful server exchange — clears the failure streak and the
 * watchdog's offline/backoff escalation. */
static void note_server_ok(void)
{
	net_fail_streak = 0;
	sys_flag_set(SYS_FLAG_SERVER_ONLINE);
	net_watchdog_ok();
}

/* Record a server-reachability failure (a poll/fetch/input request failed).
 *
 * Distinguishes the two survivable outage classes the user cares about:
 *   - Wi-Fi instability: SYS_FLAG_WIFI_READY is clear. The main loop blocks on
 *     that flag at its top and llss_on_wifi(DISCONNECTED) already owns the
 *     Wi-Fi-specific UI + session teardown, so we do nothing here.
 *   - Server instability: Wi-Fi is up but the server is unreachable/erroring.
 *     Report it distinctly, and after a few consecutive failures soft-reset the
 *     session + DNS resolver (the storm can wedge the resolver so lookups keep
 *     returning "no results" even after the server returns).
 *
 * The escalating self-heal (soft reset → reboot, minutes-scale exponential
 * backoff) is delegated to the connectivity watchdog (net_watchdog_fail). */
static void note_server_failure(void)
{
	if (!(sys_flag_get() & SYS_FLAG_WIFI_READY)) {
		return; /* Wi-Fi down — not a server problem; wifi_prov path owns it. */
	}

	sys_flag_clear(SYS_FLAG_SERVER_ONLINE);
	net_fail_streak++;

	if (net_fail_streak == 1) {
		ui_log_push("Servidor indisponivel, tentando reconectar...");
		/* Don't clobber a displayed frame; show the status only when the
		 * screen has nothing yet (matches the other status-screen calls).
		 * A frame-overlay "offline" banner is the deferred UX feature. */
		if (!last_frame_id[0]) {
			ui_server_status_show(ICON_WARNING,
				"Servidor indisponivel",
				"Wi-Fi conectado, mas sem resposta do servidor. Tentando reconectar.");
		}
	}

	net_watchdog_fail();
}

/* =========================================================================
 * State handlers — each runs one step and returns
 * ========================================================================= */

static enum app_state do_register(void)
{
	ui_log_push("Conectando ao servidor, registrando...");
	ui_server_status_show(ICON_APP_REGISTER,
		"Registrando dispositivo",
		"Conectando este dispositivo ao servico.");

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
		ui_log_push("Ja registrado, aguardando aprovacao");
		return STATE_WAITING_AUTHORIZATION;
	}

	LOG_ERR("Registration failed: %d — retry in 10s", rc);
	ui_log_push("Servidor inacessivel, tentando em 10s...");
	k_msleep(10000);
	return STATE_REGISTERING;
}

static enum app_state do_wait_authorization(void)
{
	/* If we reached WAITING without ever having a device_secret, we came
	 * here from a 409 on /register: the server already has a record for
	 * this hardware_id but we hold no secret, so there is nothing to
	 * authenticate with.  Authorizing in the portal will NOT help — the
	 * admin must REMOVE/reset the device record, after which our next
	 * register attempt succeeds cleanly.  Show that instruction and retry
	 * /register periodically so we auto-recover once the record is cleared. */
	if (!device_secret[0]) {
		ui_log_push("Ja registrado. Peca ao admin para remover o dispositivo.");
		ui_server_status_show(ICON_PENDING,
			"Ja registrado no servidor",
			"Peca ao admin para remover este dispositivo, depois aguarde.");
		LOG_INF("No device_secret stored — retrying /register in 30s");
		k_msleep(30000);
		return STATE_REGISTERING;
	}

	ui_log_push("Aguardando autorizacao no portal admin");
	ui_server_status_show(ICON_PENDING,
		"Aprovacao necessaria",
		"Autorize este dispositivo no portal admin para continuar.");

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
		ui_log_push("Dispositivo rejeitado, contate o admin");
		return STATE_ERROR;
	}

	LOG_ERR("Auth check failed (%d) — retry in 10s", rc);
	ui_log_push("Servidor inacessivel, tentando...");
	k_msleep(10000);
	return STATE_WAITING_AUTHORIZATION;
}

static enum app_state do_refresh(void)
{
	if (!last_frame_id[0]) {
		ui_server_status_show(ICON_VPN_KEY,
			"Entrando",
			"Restaurando o acesso ao servico.");
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
	ui_log_push("Autenticando...");
	ui_server_status_show(ICON_LOCK_CLOCK,
		"Verificando acesso",
		"Confirmando que o dispositivo pode receber telas.");

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
			auth_reject_streak = 0;
			LOG_INF("Authenticated — polling");
			return STATE_POLLING;
		}
		LOG_ERR("Token exchange failed %d — retry in 5s", rc);
		k_msleep(5000);
		return STATE_AUTHENTICATING;
	} else if (rc == 0 && auth_status == LLSS_AUTH_PENDING) {
		return STATE_WAITING_AUTHORIZATION;
	} else if (rc == 0 && (auth_status == LLSS_AUTH_REJECTED ||
			       auth_status == LLSS_AUTH_REVOKED)) {
		/* Deliberate server-side reject/revoke (HTTP 200 with an explicit
		 * status). Respect the admin decision — do NOT re-register. */
		session_close_on_net_error(-EACCES);
		sys_flag_clear(SYS_FLAG_LLSS_AUTHORIZED);
		ui_log_push("Dispositivo rejeitado, contate o admin");
		return STATE_ERROR;
	} else if (rc == -EACCES) {
		/* HTTP 401/403: the server does not recognise our credentials at
		 * all — most likely the device record was deleted server-side,
		 * leaving us with a stale device_id/secret we can never use. After
		 * a few consecutive failures (guard against a transient 401),
		 * self-heal: wipe local credentials and re-register from scratch. */
		session_close_on_net_error(-EACCES);
		sys_flag_clear(SYS_FLAG_LLSS_AUTHORIZED);
		if (++auth_reject_streak < LLSS_CRED_REJECT_RESET_THRESHOLD) {
			LOG_WRN("Credentials rejected (%d/%d) — retry in 5s",
				auth_reject_streak,
				LLSS_CRED_REJECT_RESET_THRESHOLD);
			k_msleep(5000);
			return STATE_AUTHENTICATING;
		}
		LOG_WRN("Credentials no longer valid — wiping and re-registering");
		ui_log_push("Credenciais invalidas, registrando novamente...");
		wipe_local_credentials();
		return STATE_REGISTERING;
	}

	session_close_on_net_error(rc);
	LOG_ERR("Authenticate error %d — retry in 5s", rc);
	k_msleep(5000);
	return STATE_AUTHENTICATING;
}

/*
 * Send one button event. On every exit that will NOT produce a new frame we
 * must drop the press-feedback overlay: it is otherwise only cleared when a
 * frame lands, so a NO_CHANGE answer (the hold-hint/notice case) or a failure
 * would leave the button stuck looking pressed.
 */
static enum app_state do_send_input(const struct button_event *bev)
{
	int32_t rc = session_ensure();

	if (rc < 0) {
		ui_press_feedback_clear();
		k_msleep(5000);
		return STATE_POLLING;
	}

	struct llss_input_response input = {0};

	rc = llss_send_input(access_token, device_id,
			     ui_btn_llss_name(bev->btn),
			     ui_evt_llss_name(bev->evt), &input);
	session_close_on_net_error(rc);

	if (rc == -EACCES) {
		ui_press_feedback_clear();
		return STATE_REFRESHING;
	}
	if (rc != 0) {
		/* rc < 0 = net error; rc > 0 = HTTP error status — both are failures
		 * (don't read the unparsed response below). */
		LOG_ERR("send_input error %d", rc);
		note_server_failure();
		ui_press_feedback_clear();
		return STATE_POLLING;
	}

	note_server_ok();

	/* Server-driven transient popup (e.g. "hold the button to offer a draw").
	 * Independent of any frame — it commonly rides a NO_CHANGE, so show it
	 * before the status branch returns. No-op when empty / device menu is up. */
	if (input.notice[0]) {
		ui_notice_show(input.notice);
	}

	strncpy(latest_top_strip_id, input.top_strip_id,
		sizeof(latest_top_strip_id) - 1);
	latest_top_strip_id[sizeof(latest_top_strip_id) - 1] = '\0';
	strncpy(latest_bottom_strip_id, input.bottom_strip_id,
		sizeof(latest_bottom_strip_id) - 1);
	latest_bottom_strip_id[sizeof(latest_bottom_strip_id) - 1] = '\0';
	latest_top_enabled_mask    = input.top_enabled_mask;
	latest_bottom_enabled_mask = input.bottom_enabled_mask;
	latest_full_refresh        = input.full_refresh;

	if (input.status == LLSS_INPUT_NEW_FRAME && input.frame_id[0]) {
		strncpy(last_frame_id, input.frame_id,
			sizeof(last_frame_id) - 1);
		return STATE_FETCHING_FRAME;
	} else if (input.status == LLSS_INPUT_POLL && input.poll_after_ms > 0) {
		poll_interval_ms = CLAMP(input.poll_after_ms,
					 CONFIG_LLSS_MIN_POLL_MS,
					 CONFIG_LLSS_MAX_POLL_MS);
	}

	/* No frame is coming (NO_CHANGE / POLL) — un-press the button. The
	 * NEW_FRAME path above returns early; display_frame_locked() clears the
	 * overlay there as part of drawing the new frame. */
	ui_press_feedback_clear();
	return STATE_POLLING;
}

/* Idle wait that stays responsive: block up to `timeout_ms` ON THE BUTTON
 * QUEUE instead of sleeping blind.  A press wakes us instantly and is sent
 * right away (the old k_msleep made every press wait out up to a full poll
 * interval before it was even forwarded).  On timeout we return to POLLING so
 * the routine poll still runs at the server's cadence. */
static enum app_state wait_button_or_poll(int32_t timeout_ms)
{
	struct button_event bev;

	/* This blocking wait is intentional idle — don't let the liveness
	 * watchdog count it as a hang. */
	sys_wdt_idle(llss_wdt);
	int got = k_msgq_get(&btn_queue, &bev, K_MSEC(timeout_ms));

	sys_wdt_alive(llss_wdt);

	if (got == 0) {
		LOG_INF("Button: %s %s",
			ui_btn_llss_name(bev.btn), ui_evt_llss_name(bev.evt));
		return do_send_input(&bev);
	}
	return STATE_POLLING;
}

static enum app_state do_poll(void)
{
	int32_t rc = session_ensure();

	if (rc < 0) {
		/* Couldn't even open the session (DNS/connect failed). Back off via
		 * the (never-zero) poll interval rather than spinning. */
		note_server_failure();
		return STATE_POLLING;
	}

	struct llss_device_state state = {0};

	rc = llss_get_device_state(access_token, device_id,
				   last_frame_id, &state);
	session_close_on_net_error(rc);

	if (rc == -EACCES) {
		return STATE_REFRESHING;
	}
	if (rc != 0) {
		/* rc < 0 = network error; rc > 0 = HTTP error status (e.g. 502).
		 * BOTH are failures — do NOT fall through and copy poll_after_ms out
		 * of the unparsed (zeroed) state, which made poll_interval_ms 0 and
		 * spun the loop at ~40 reconnects/s, starving net buffers and wedging
		 * DNS. Keep the last good interval and back off. (do_request now
		 * KEEPS the persistent session through HTTP errors and only closes
		 * on a real transport failure, so the next poll reuses the same
		 * connection instead of churning a new one each 502.) */
		LOG_ERR("Poll error %d", rc);
		note_server_failure();
		return STATE_POLLING;
	}

	note_server_ok();
	/* Only ever set from a *successful* parse, and clamp defensively so the
	 * poll interval can never be 0 regardless of what the server sends. */
	poll_interval_ms = CLAMP(state.poll_after_ms,
				 CONFIG_LLSS_MIN_POLL_MS, CONFIG_LLSS_MAX_POLL_MS);

	strncpy(latest_top_strip_id, state.top_strip_id,
		sizeof(latest_top_strip_id) - 1);
	latest_top_strip_id[sizeof(latest_top_strip_id) - 1] = '\0';
	strncpy(latest_bottom_strip_id, state.bottom_strip_id,
		sizeof(latest_bottom_strip_id) - 1);
	latest_bottom_strip_id[sizeof(latest_bottom_strip_id) - 1] = '\0';
	latest_top_enabled_mask    = state.top_enabled_mask;
	latest_bottom_enabled_mask = state.bottom_enabled_mask;
	latest_full_refresh        = state.full_refresh;

	switch (state.action) {
	case LLSS_ACTION_FETCH_FRAME:
		if (state.frame_id[0] && !ui_has_server_frame()) {
			ui_server_status_show(ICON_SYNC,
				"Carregando tela",
				"Recebendo o conteudo mais recente do servidor.");
		}
		if (state.frame_id[0]) {
			strncpy(last_frame_id, state.frame_id,
				sizeof(last_frame_id) - 1);
			return STATE_FETCHING_FRAME;
		}
		if (!last_frame_id[0]) {
			ui_server_status_show(ICON_SCREEN,
				"Aguardando conteudo",
				"O dispositivo esta conectado e aguardando a primeira tela.");
		}
		return STATE_POLLING;
	case LLSS_ACTION_SLEEP:
		if (!last_frame_id[0]) {
			ui_server_status_show(ICON_MONITOR,
				"Aguardando conteudo",
				"O dispositivo esta conectado e aguardando a primeira tela.");
		}
		return STATE_SLEEPING;
	default:
		if (!last_frame_id[0]) {
			ui_server_status_show(ICON_MONITOR,
				"Aguardando conteudo",
				"O dispositivo esta conectado e aguardando a primeira tela.");
		}
		return STATE_POLLING;
	}
}

/* Fetch one pressed strip into the cache if it isn't there yet. The session
 * is assumed to be open; runs in the LLSS thread (no extra worker), so the
 * cost is paid between polls — never on the press handler's critical path. */
static void prefetch_strip(const char *strip_id)
{
	if (!strip_id || !strip_id[0]) {
		return;
	}
	if (llss_strip_cache_get(strip_id, NULL)) {
		return;
	}

	size_t len = 0;
	int rc = llss_fetch_pressed_strip(access_token, device_id, strip_id,
					  strip_fetch_buf,
					  sizeof(strip_fetch_buf), &len);
	if (rc == 0 && len > 0) {
		llss_strip_cache_put(strip_id, strip_fetch_buf, len);
	} else if (rc != -ENOENT) {
		LOG_WRN("strip prefetch %s failed: %d", strip_id, rc);
	}
}

static void prefetch_pending_strips(void)
{
	prefetch_strip(latest_top_strip_id);
	prefetch_strip(latest_bottom_strip_id);

	/* Upgrade the per-slot press-feedback variants now that strips may
	 * be in cache. Bands with no cached strip keep the INVERT variants.
	 * The same update also carries the latest server-side enabled masks,
	 * overriding the capture-time heuristic for any band the server
	 * actually advertised.
	 *
	 * Latched, not applied here: taking lvgl_mutex on this thread would
	 * block the whole network state machine behind an in-flight e-ink
	 * refresh (~0.5 s), re-serialising the HTTP round-trip against the
	 * panel. The display thread applies it under the lock it already holds. */
	ui_press_feedback_update_strips(latest_top_strip_id,
					latest_bottom_strip_id,
					latest_top_enabled_mask,
					latest_bottom_enabled_mask);
}

static enum app_state do_fetch_frame(void)
{
	ui_log_push("Baixando tela...");
	if (!ui_has_server_frame()) {
		ui_server_status_show(ICON_SYNC,
			"Carregando tela",
			"Recebendo o conteudo mais recente do servidor.");
	}

	int32_t rc = session_ensure();

	if (rc < 0) {
		note_server_failure();
		return STATE_POLLING;
	}

	/* Fetch straight into a display pipeline buffer — no copy. */
	size_t cap = 0;
	uint8_t *dst = display_frame_write_buf(&cap);
	size_t png_len = 0;

	/* Guard the destination: under net-buffer/heap exhaustion the pipeline can
	 * hand back no buffer. Passing NULL/0 into the fetch (which writes the HTTP
	 * body straight here) would be an unchecked deref — bail cleanly instead. */
	if (dst == NULL || cap == 0) {
		LOG_ERR("Frame write buffer unavailable (dst=%p cap=%zu)",
			(void *)dst, cap);
		return STATE_POLLING;
	}

	rc = llss_fetch_frame(access_token, device_id, last_frame_id,
			      dst, cap, &png_len);
	session_close_on_net_error(rc);

	if (rc == -EACCES) {
		return STATE_REFRESHING;
	}
	if (rc != 0) {
		/* rc < 0 = net error / empty body; rc > 0 = HTTP error status. */
		LOG_ERR("Fetch frame failed: %d", rc);
		note_server_failure();
		return STATE_POLLING;
	}

	note_server_ok();

#if defined(CONFIG_LLSS_PATTERN_TEST)
	/* CP1 — verify the bytes the HTTP layer delivered, before the buffer is
	 * handed to the display pipeline. Exercises transport + the do_request()
	 * recv_buf/ctx.buf aliasing. `dst` is the body (slot + I1 palette). The
	 * canary lives in the slot slack right after the 48000-byte body. */
	(void)pattern_verify(dst, png_len, "CP1-fetch");
	/* http_response_cb writes one NUL terminator at dst[png_len]; that byte
	 * legitimately lands on the first slack byte, so skip it and check the
	 * rest of the slack for real overruns. */
	if (png_len == PATTERN_BYTES && cap > PATTERN_BYTES + 1) {
		(void)pattern_canary_verify(dst + PATTERN_BYTES + 1,
					    cap - PATTERN_BYTES - 1, "CP1-canary");
	}
#endif

	if (png_len > 0) {
		int32_t drc = display_frame_submit(png_len, latest_full_refresh);
		latest_full_refresh = false; /* one-shot hint */

		if (drc < 0) {
			LOG_ERR("Frame submit failed: %d", drc);
		}
	}

	/* Frame is on the wire to the panel — opportunistically pull the
	 * pressed strips so the next press has local feedback. Cache hits
	 * are no-ops; misses run sequentially in this thread (acceptable —
	 * total time stays well under one poll interval). */
	prefetch_pending_strips();

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

	llss_strip_cache_init();

	int32_t rc = llss_client_init();

	if (rc) {
		LOG_ERR("llss_client_init: %d", rc);
		ui_log_push("Falha LLSS, verifique cert CA");
		app_state = STATE_ERROR;
	} else {
		app_state = device_id[0] ? STATE_REFRESHING : STATE_REGISTERING;
	}

	/* Network thread: generous timeout covers a worst-case poll+fetch; the long
	 * idle waits below are excused via sys_wdt_idle(). */
	llss_wdt = sys_wdt_register("llss", 60000);

	while (true) {
		/* try / fail / wait.  If WiFi is up and clock is valid we
		 * proceed; if either drops, we block here until both return.
		 * No polling, no special "I'm waiting for WiFi" state.
		 *
		 * 30 s timeout is just so a stuck wait surfaces in the serial
		 * log instead of being silent — the wait is still blocking
		 * for as long as needed. */
		uint32_t needed = SYS_FLAG_WIFI_READY | SYS_FLAG_TIME_VALID;

		sys_wdt_idle(llss_wdt);
		uint32_t got = sys_flag_wait_all(needed, K_SECONDS(30));

		sys_wdt_alive(llss_wdt);

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
		{
			struct button_event bev;
			/* Service an already-queued button before issuing another /state
			 * poll so user input never waits behind an avoidable round-trip. */
			if (k_msgq_get(&btn_queue, &bev, K_NO_WAIT) == 0) {
				LOG_INF("BTNTRACE llss consume code=%d evt=%d t=%u",
					(int)bev.btn, (int)bev.evt, k_uptime_get_32());
				LOG_INF("Button: %s %s",
					ui_btn_llss_name(bev.btn),
					ui_evt_llss_name(bev.evt));
				app_state = do_send_input(&bev);
				break;
			}
			if (app_state != STATE_POLLING) {
				break; /* input sent us to another state */
			}
			app_state = do_poll();
			/* Poll found nothing to fetch -> idle on the button queue for
			 * one interval so a press is serviced instantly instead of
			 * after a blind sleep. */
			if (app_state == STATE_POLLING) {
				app_state = wait_button_or_poll(poll_interval_ms);
			}
			break;
		}
		case STATE_FETCHING_FRAME:        /* -> POLLING | REFRESHING */
			app_state = do_fetch_frame();
			break;
		case STATE_SLEEPING:              /* -> POLLING | FETCHING_FRAME | REFRESHING */
			LOG_INF("Sleeping for %d ms", poll_interval_ms);
			ui_log_push("Dormindo...");
			app_state = wait_button_or_poll(poll_interval_ms);
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
