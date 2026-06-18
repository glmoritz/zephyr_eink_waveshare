#ifndef LLSS_NET_WATCHDOG_H_
#define LLSS_NET_WATCHDOG_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Connectivity watchdog — server-reachability self-heal escalation.
 *
 * Driven by the LLSS poll/fetch path (only while Wi-Fi is up; Wi-Fi loss is a
 * separate, wifi_prov-owned concern). Tracks how long the device has gone
 * without a successful server exchange and escalates:
 *
 *   1. SOFT RESET  — llss_client_net_reset() (session + DNS resolver reinit) at
 *      an exponentially-backed-off cadence starting in minutes. Cheap; clears a
 *      storm-wedged resolver.
 *   2. REBOOT      — last resort once offline far longer, also exponential and
 *      capped (default 24 h). Counters persist in RTC RAM so the escalation
 *      survives the reboot it triggers (otherwise every reset restarts the
 *      schedule).
 *
 * DRY-RUN (CONFIG_LLSS_WATCHDOG_DRY_RUN, default y while debugging): step 2 logs
 * "WATCHDOG: would reset now ..." and advances its schedule instead of actually
 * rebooting, so the escalation is observable without disrupting a capture.
 *
 * NOT yet wired: the Zephyr Task WDT + ESP32 hardware WDT per-thread liveness
 * layer, and Wi-Fi-down reboot — see memory project_system_watchdog. This is the
 * connectivity-beat layer only.
 */

/* Call on every successful server exchange (poll/fetch/input). Resets the
 * offline timer and the soft-reset/reboot backoff schedules. */
void net_watchdog_ok(void);

/* Call on every failed server exchange while Wi-Fi is up. Advances the
 * escalation (soft reset, then would-reset/reboot) per the backoff schedule. */
void net_watchdog_fail(void);

#ifdef __cplusplus
}
#endif

#endif /* LLSS_NET_WATCHDOG_H_ */
