#ifndef LLSS_CLIENT_H_
#define LLSS_CLIENT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Enumerations matching the OpenAPI spec
 * ========================================================================= */

enum llss_auth_status {
	LLSS_AUTH_PENDING = 0,
	LLSS_AUTH_AUTHORIZED,
	LLSS_AUTH_REJECTED,
	LLSS_AUTH_REVOKED,
	LLSS_AUTH_UNKNOWN,
};

enum llss_action {
	LLSS_ACTION_NOOP = 0,
	LLSS_ACTION_FETCH_FRAME,
	LLSS_ACTION_SLEEP,
	LLSS_ACTION_ERROR,
};

enum llss_input_result {
	LLSS_INPUT_NEW_FRAME = 0,
	LLSS_INPUT_NO_CHANGE,
	LLSS_INPUT_POLL,
	LLSS_INPUT_ERROR,
};

/* Content-addressable strip id length (opaque token, ≤32 chars + NUL). */
#define LLSS_STRIP_ID_MAX 33

/* =========================================================================
 * Data structures
 * ========================================================================= */

struct llss_device_state {
	enum llss_action action;
	char             frame_id[64];
	int32_t          poll_after_ms;
	/* Pressed-state strip ids for the current frame (protocol extension).
	 * Empty string when the active instance did not upload one. */
	char             top_strip_id[LLSS_STRIP_ID_MAX];
	char             bottom_strip_id[LLSS_STRIP_ID_MAX];
	/* 8-bit per-band enabled-slot bitmask. -1 = server did not advertise
	 * one (device falls back to the local all-white-slot heuristic). */
	int32_t          top_enabled_mask;
	int32_t          bottom_enabled_mask;
	/* When true, the device should drive a full e-ink refresh on this
	 * frame instead of partial. HLSS sets it for view-mode toggles and
	 * board-state changes (move applied, opponent move). */
	bool             full_refresh;
};

struct llss_input_response {
	enum llss_input_result status;
	char                   frame_id[64];
	int32_t                poll_after_ms;
	/* User-facing transient popup text (server "notice"). Empty when the
	 * server sent none. The device flashes it for a couple of seconds; it is
	 * independent of any frame and can arrive with a NO_CHANGE status. */
	char                   notice[128];
	char                   top_strip_id[LLSS_STRIP_ID_MAX];
	char                   bottom_strip_id[LLSS_STRIP_ID_MAX];
	int32_t                top_enabled_mask;    /* -1 = unspecified */
	int32_t                bottom_enabled_mask; /* -1 = unspecified */
	bool                   full_refresh;
};

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialise the LLSS HTTP client and register CA certificate.
 *
 * Must be called once after WiFi is connected and before any other
 * llss_* function.
 *
 * @return 0 on success, negative errno on failure.
 */
int llss_client_init(void);

/**
 * @brief Open a persistent TLS session to the LLSS server (Phase A).
 *
 * Opens one TLS connection that is reused by all subsequent llss_* API
 * calls until llss_session_close() is called.  This avoids a full TLS
 * handshake (~3 s on ESP32-S3) for every API call in a wake cycle.
 *
 * Typical use in a wake cycle:
 *   llss_session_open();
 *   llss_get_device_state(...);
 *   llss_fetch_frame(...);
 *   llss_session_close();
 *
 * llss_client_init() must have been called first.  If a session is
 * already open this is a no-op.
 *
 * @return 0 on success, negative errno on failure.
 */
int llss_session_open(void);

/**
 * @brief Close the persistent TLS session opened by llss_session_open().
 *
 * Safe to call even if no session is open (no-op).
 */
void llss_session_close(void);

/**
 * @brief Soft network reset for the server-instability self-heal path.
 *
 * Drops the cached server address, closes the persistent session, and fully
 * reinitialises the DNS resolver (close + reinit with the same servers). A
 * prolonged reconnect storm can wedge the resolver so every lookup returns
 * "no results" even after the server is reachable again; this clears that.
 * Safe to call any time after llss_client_init().
 */
void llss_client_net_reset(void);

/**
 * @brief Register this device with the LLSS server (unauthenticated).
 *
 * Sends device hardware_id and display capabilities; returns device_id
 * and device_secret (must be persisted to NV storage immediately).
 *
 * @param hardware_id       Unique hardware string (e.g. MAC address hex).
 * @param device_id_out     Buffer for returned device_id  (≥65 bytes).
 * @param device_secret_out Buffer for returned device_secret (≥65 bytes).
 * @return 0 on success (HTTP 201 or 409 if already registered), negative
 *         errno or positive HTTP status code on other errors.
 */
int llss_register(const char *hardware_id,
		  char *device_id_out, char *device_secret_out);

/**
 * @brief Authenticate device and obtain a refresh token.
 *
 * Unauthenticated endpoint. If the device is pending admin authorization,
 * auth_status_out will be LLSS_AUTH_PENDING and refresh_token_out will be
 * empty.
 *
 * @param hardware_id        Unique hardware string.
 * @param device_secret      Secret from registration.
 * @param auth_status_out    Filled with current LLSS auth_status.
 * @param refresh_token_out  Buffer for refresh_token (≥512 bytes); may be
 *                           empty when status is PENDING.
 * @return 0 on success (HTTP 200), -EACCES on 401/403, negative errno on error.
 */
int llss_authenticate(const char *hardware_id, const char *device_secret,
		      enum llss_auth_status *auth_status_out,
		      char *refresh_token_out);

/**
 * @brief Exchange a refresh token for a new short-lived access token.
 *
 * @param refresh_token    Valid 30-day refresh token.
 * @param access_token_out Buffer for new access token (≥512 bytes).
 * @return 0 on success, -EACCES on 401/403, negative errno on error.
 */
int llss_refresh_access_token(const char *refresh_token,
			      char *access_token_out);

/**
 * @brief Renew the refresh token using the current access token.
 *
 * Call periodically (e.g. every 15 days) to avoid refresh token expiry.
 *
 * @param access_token       Current Bearer access token.
 * @param refresh_token_out  Buffer for new refresh token (≥512 bytes).
 * @return 0 on success, -EACCES on 401/403, negative errno on error.
 */
int llss_renew_refresh_token(const char *access_token,
			     char *refresh_token_out);

/**
 * @brief Poll device state (heartbeat endpoint).
 *
 * Reports last displayed frame_id; server responds with next action.
 *
 * @param access_token   Valid Bearer access token.
 * @param device_id      Registered device ID.
 * @param last_frame_id  Last successfully displayed frame ID (may be "").
 * @param state_out      Filled with server action + frame_id + poll_after_ms.
 * @return 0 on success, -EACCES on 401, negative errno on error.
 */
int llss_get_device_state(const char *access_token, const char *device_id,
			  const char *last_frame_id,
			  struct llss_device_state *state_out);

/**
 * @brief Fetch a PNG frame from the server into a caller-provided buffer.
 *
 * The HTTP body is written directly into @p dst (no internal copy), so the
 * caller can hand a display pipeline buffer straight to the network stack.
 *
 * @param access_token  Valid Bearer access token.
 * @param device_id     Registered device ID.
 * @param frame_id      Frame ID to fetch.
 * @param dst           Destination buffer for the PNG body.
 * @param dst_size      Capacity of @p dst in bytes.
 * @param len_out       Set to bytes written on success.
 * @return 0 on success, -EACCES on 401, -ENODATA on empty body,
 *         negative errno or positive HTTP status on other errors.
 */
int llss_fetch_frame(const char *access_token, const char *device_id,
		     const char *frame_id,
		     uint8_t *dst, size_t dst_size, size_t *len_out);

/**
 * @brief Fetch a pressed-state button strip by content id (protocol extension).
 *
 * Requests `GET /devices/{id}/strips/{strip_id}` where @p strip_id is the
 * opaque content-hash token advertised by the server in
 * `llss_device_state.top_strip_id` / `bottom_strip_id` (or the equivalent
 * fields in `llss_input_response`). Strips are immutable for a given id; the
 * caller is expected to cache by id and skip the fetch on a hit.
 *
 * A server (or instance) with no pressed strip for that id returns 404,
 * reported as -ENOENT — the caller simply skips press feedback.
 *
 * @param access_token  Valid Bearer access token.
 * @param device_id     Registered device ID.
 * @param strip_id      Opaque content-hash id (≤32 chars).
 * @param dst           Destination buffer for the body.
 * @param dst_size      Capacity of @p dst in bytes.
 * @param len_out       Set to bytes written on success.
 * @return 0 on success, -ENOENT if no pressed strip is available (404),
 *         -EACCES on 401/403, -ENODATA on empty body, negative errno or
 *         positive HTTP status on other errors.
 */
int llss_fetch_pressed_strip(const char *access_token, const char *device_id,
			     const char *strip_id,
			     uint8_t *dst, size_t dst_size, size_t *len_out);

/**
 * @brief Send a button input event to the server.
 *
 * @param access_token  Valid Bearer access token.
 * @param device_id     Registered device ID.
 * @param button_name   Button ID string: "BTN_1"–"BTN_8", "ENTER", "ESC",
 *                      "HL_LEFT", "HL_RIGHT".
 * @param event_type    "PRESS", "RELEASE", or "LONG_PRESS".
 * @param resp_out      Filled with server response (status, optional frame_id).
 * @return 0 on success, -EACCES on 401, negative errno on error.
 */
int llss_send_input(const char *access_token, const char *device_id,
		    const char *button_name, const char *event_type,
		    struct llss_input_response *resp_out);

#ifdef __cplusplus
}
#endif

#endif /* LLSS_CLIENT_H_ */
