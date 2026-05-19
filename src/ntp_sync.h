#ifndef NTP_SYNC_H_
#define NTP_SYNC_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Restore system clock from the RTC at boot.
 *
 * Reads the PCF85063A and calls clock_settime(CLOCK_REALTIME).
 * Does nothing if the RTC has never been set (year < 2024).
 */
void ntp_sync_restore_from_rtc(void);

/**
 * @brief Report whether CLOCK_REALTIME looks valid for TLS cert checks.
 *
 * Returns true when the current realtime clock is at or after 2024-01-01 UTC.
 */
bool ntp_sync_time_is_valid(void);

/**
 * @brief Perform a single NTP sync, update the RTC and system clock.
 *
 * Resolves CONFIG_LLSS_NTP_SERVER via DNS (heap-free), queries the SNTP
 * server, writes the result to the PCF85063A, and sets CLOCK_REALTIME.
 *
 * @return 0 on success, negative errno on failure.
 */
int ntp_sync_once(void);

/**
 * @brief Start the periodic NTP sync work item.
 *
 * Schedules the first sync immediately and repeats every
 * CONFIG_LLSS_NTP_SYNC_INTERVAL_S seconds thereafter.
 * Runs in the system workqueue.
 */
void ntp_sync_start(void);

#ifdef __cplusplus
}
#endif

#endif /* NTP_SYNC_H_ */
