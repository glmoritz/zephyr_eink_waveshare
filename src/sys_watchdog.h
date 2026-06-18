#ifndef LLSS_SYS_WATCHDOG_H_
#define LLSS_SYS_WATCHDOG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * System liveness watchdog (CONFIG_LLSS_HW_WATCHDOG).
 *
 * A supervisor thread watches registered threads. Each thread marks itself
 * "alive" while doing work and "idle" right before an intentional blocking
 * sleep — the supervisor IGNORES idle threads, so a thread legitimately parked
 * on a sem/sleep is never mistaken for a hang. A thread that stays ACTIVE past
 * its timeout without a heartbeat is considered hung.
 *
 * When armed (CONFIG_LLSS_HW_WATCHDOG_DRY_RUN=n) the supervisor feeds the ESP32
 * hardware WDT while everything is healthy, so a hung thread — or a hung
 * supervisor — triggers a hardware reset. In dry-run (default while debugging)
 * it only logs "WATCHDOG: would reset now — '<thread>' starved" and never
 * resets, so panics still halt for capture.
 *
 * Typical use:
 *   static int wd = sys_wdt_register("llss", 90000);
 *   for (;;) {
 *       sys_wdt_idle(wd);                 // about to block
 *       item = k_msgq_get(..., K_FOREVER);
 *       sys_wdt_alive(wd);                // working again
 *       ... process ...
 *   }
 *
 * All calls are no-ops when CONFIG_LLSS_HW_WATCHDOG=n.
 */

/* Register a watched task. @timeout_ms = max time it may stay ACTIVE without a
 * heartbeat before it's deemed hung. Returns a handle (>=0) or -1 if disabled
 * or the table is full. Call once per thread. */
int sys_wdt_register(const char *name, uint32_t timeout_ms);

/* Heartbeat: the thread is alive and working. Resets its deadline and marks it
 * active. Call on each work cycle / right after waking from a blocking wait. */
void sys_wdt_alive(int handle);

/* The thread is about to block intentionally (sleep / K_FOREVER wait). The
 * supervisor ignores it until the next sys_wdt_alive(). */
void sys_wdt_idle(int handle);

#ifdef __cplusplus
}
#endif

#endif /* LLSS_SYS_WATCHDOG_H_ */
