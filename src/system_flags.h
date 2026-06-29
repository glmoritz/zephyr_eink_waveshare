#ifndef SYSTEM_FLAGS_H_
#define SYSTEM_FLAGS_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

/*
 * System-wide event flags — single source of truth for cross-subsystem
 * readiness state.  Producers set/clear on transition; consumers block on
 * the bits they need via k_event_wait_all / k_event_wait.  Flags reflect
 * *current* readiness, never historical events.
 *
 * Add bits below as needed.  Keep them mutually intelligible: the bit
 * comment should describe what is true while the bit is set.
 */

/* WiFi association complete, an IPv4 (and optionally IPv6) address is bound.
 * Set by the WiFi state machine when it enters STA-connected state; cleared
 * on disconnect, link drop, or while in provisioning mode. */
#define SYS_FLAG_WIFI_READY         BIT(0)

/* WiFi is in provisioning mode (soft-AP + captive portal up).  Set while
 * the user is expected to point a phone/laptop at the device to configure
 * credentials.  Mutually exclusive with SYS_FLAG_WIFI_READY. */
#define SYS_FLAG_WIFI_PROVISIONING  BIT(1)

/* CLOCK_REALTIME has been validated against a trusted source (NTP or RTC
 * cell with a known-recent timestamp).  Set by the clock state machine.
 * Cleared if the source becomes stale or fails revalidation. */
#define SYS_FLAG_TIME_VALID         BIT(2)

/* LLSS backend has confirmed this device is authorized (auth_status =
 * "authorized").  Set when the most recent /auth response said so;
 * cleared on rejected/revoked.  Useful to the display thread for icons
 * and to gate higher-level features. */
#define SYS_FLAG_LLSS_AUTHORIZED    BIT(3)

/* The server is currently reachable: set after a successful poll/fetch/input
 * exchange, cleared on a server-reachability failure (while Wi-Fi is up) or on
 * Wi-Fi loss. Distinct from LLSS_AUTHORIZED (an authorization verdict that
 * persists across transient outages) — this tracks live connectivity so the UI
 * can distinguish "online" / "Wi-Fi lost" / "server unreachable". */
#define SYS_FLAG_SERVER_ONLINE      BIT(4)

extern struct k_event system_flags;

/*
 * Edge-trigger notification raised on every flag mutation.  Consumers that
 * need to "wake when anything changes" (e.g. the display status renderer)
 * use `k_poll(K_POLL_TYPE_SIGNAL, &system_flags_changed)` and then snapshot
 * the level state via sys_flag_get().  This complements the level-triggered
 * k_event waits used by workers that block on specific bits.
 */
extern struct k_poll_signal system_flags_changed;

/*
 * I/O in-flight refcount.  Any subsystem that starts a transfer that must
 * complete cleanly before deep-sleep entry calls sys_io_acquire(); when the
 * transfer ends (success OR failure), it calls sys_io_release().  The sleep
 * coordinator treats `count == 0` as "no holders, deep sleep is safe".
 *
 * Implemented as an atomic counter so two holders can coexist without races.
 */
extern atomic_t system_io_inflight;

/* ---- thin wrappers so callers don't reach into the kernel object ---- */

static inline void sys_flag_set(uint32_t bits)
{
	/* k_event_post(bits) OR-s the bits in without touching other bits.
	 * Do NOT use k_event_set() here — that one *replaces* the whole event
	 * mask with `bits`, which silently wipes every other flag a different
	 * producer had previously set.  (Real bug we tripped on: NTP set
	 * TIME_VALID, then WiFi set WIFI_READY with k_event_set, blanking
	 * TIME_VALID and trapping the LLSS thread on its wait_all forever.) */
	k_event_post(&system_flags, bits);
	k_poll_signal_raise(&system_flags_changed, 0);
}

static inline void sys_flag_clear(uint32_t bits)
{
	k_event_clear(&system_flags, bits);
	k_poll_signal_raise(&system_flags_changed, 0);
}

static inline uint32_t sys_flag_get(void)
{
	return k_event_test(&system_flags, ~0U);
}

/* Wait until *all* requested bits are set.  Returns the matched bits or 0
 * on timeout. */
static inline uint32_t sys_flag_wait_all(uint32_t bits, k_timeout_t t)
{
	return k_event_wait_all(&system_flags, bits, false, t);
}

/* Wait until *any* requested bit is set.  Returns the matched bits or 0
 * on timeout. */
static inline uint32_t sys_flag_wait_any(uint32_t bits, k_timeout_t t)
{
	return k_event_wait(&system_flags, bits, false, t);
}

/* I/O refcount helpers — see comment on system_io_inflight above. */
static inline void sys_io_acquire(void)
{
	atomic_inc(&system_io_inflight);
}

static inline void sys_io_release(void)
{
	atomic_dec(&system_io_inflight);
}

static inline bool sys_io_quiet(void)
{
	return atomic_get(&system_io_inflight) == 0;
}

#endif /* SYSTEM_FLAGS_H_ */
