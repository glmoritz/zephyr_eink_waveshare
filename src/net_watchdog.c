#include "net_watchdog.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(net_watchdog, LOG_LEVEL_INF);

#if defined(CONFIG_LLSS_WATCHDOG)

#include <stdint.h>

#include <zephyr/sys/reboot.h>

#include "llss_client.h"
#include "section_attrs.h"

/* Tunables (seconds → ms where needed). See Kconfig for meaning. */
#define SOFT_BASE_MS   ((int64_t)CONFIG_LLSS_WATCHDOG_SOFT_RESET_BASE_S * 1000)
#define SOFT_MAX_MS    ((int64_t)CONFIG_LLSS_WATCHDOG_SOFT_RESET_MAX_S * 1000)
#define REBOOT_BASE_S  ((uint32_t)CONFIG_LLSS_WATCHDOG_REBOOT_BASE_S)
#define REBOOT_MAX_S   ((uint32_t)CONFIG_LLSS_WATCHDOG_MAX_PERIOD_S)

/* Reboot backoff persists across the reboot it triggers, so a still-offline
 * device spaces successive reboots out (base, 2×, 4×, … capped) instead of
 * boot-looping. RTC slow RAM survives reboot + deep sleep. These are plain RTC
 * scalars, NOT flash/NVS targets, so RTC placement is safe here (unlike the
 * credential caches — see memory project_nvs_rtc_buffer_nomem). */
#define WDT_RTC_MAGIC 0x57444731U /* 'WDG1' */
static uint32_t rtc_wdt_magic        LLSS_RTC_NOINIT;
static uint32_t rtc_reboot_backoff_s LLSS_RTC_NOINIT;

/* Within-boot escalation state. */
static bool    initialised;
static int64_t offline_since_ms;  /* 0 = online / no current outage */
static int64_t next_soft_reset_ms;
static int64_t next_reboot_ms;
static int64_t soft_interval_ms;

static void wdt_init(void)
{
	if (initialised) {
		return;
	}
	initialised = true;

	/* Restore the reboot backoff from a prior (real) reboot, else start at base. */
	if (rtc_wdt_magic != WDT_RTC_MAGIC ||
	    rtc_reboot_backoff_s < REBOOT_BASE_S ||
	    rtc_reboot_backoff_s > REBOOT_MAX_S) {
		rtc_wdt_magic = WDT_RTC_MAGIC;
		rtc_reboot_backoff_s = REBOOT_BASE_S;
	} else if (rtc_reboot_backoff_s > REBOOT_BASE_S) {
		LOG_WRN("Resuming reboot backoff from %u s (prior reset still offline)",
			rtc_reboot_backoff_s);
	}
}

static void take_reset_action(int64_t offline_ms)
{
	uint32_t cur_s = rtc_reboot_backoff_s;
	uint32_t next_s = MIN(cur_s * 2U, REBOOT_MAX_S);

	/* Advance + persist the backoff so the *next* reset is spaced further. */
	rtc_reboot_backoff_s = next_s;
	rtc_wdt_magic = WDT_RTC_MAGIC;

#if defined(CONFIG_LLSS_WATCHDOG_DRY_RUN)
	LOG_ERR("WATCHDOG: would reset now (offline %llds); next would-reset in %u s",
		(long long)(offline_ms / 1000), next_s);
	/* No reboot — reschedule the next would-reset to show the escalation. */
	next_reboot_ms = k_uptime_get() + (int64_t)next_s * 1000;
#else
	LOG_ERR("WATCHDOG: resetting system (offline %llds); next backoff %u s",
		(long long)(offline_ms / 1000), next_s);
	k_msleep(300); /* let the log/UART flush before the reset */
	sys_reboot(SYS_REBOOT_COLD);
#endif
}

void net_watchdog_fail(void)
{
	wdt_init();

	int64_t now = k_uptime_get();

	if (offline_since_ms == 0) {
		/* Start of an outage — arm the schedules. */
		offline_since_ms = now;
		soft_interval_ms = SOFT_BASE_MS;
		next_soft_reset_ms = now + soft_interval_ms;
		next_reboot_ms = now + (int64_t)rtc_reboot_backoff_s * 1000;
		LOG_WRN("Server offline — soft reset in %lld s, %s in %u s",
			(long long)(soft_interval_ms / 1000),
			IS_ENABLED(CONFIG_LLSS_WATCHDOG_DRY_RUN)
				? "would-reset" : "reboot",
			rtc_reboot_backoff_s);
		return;
	}

	int64_t offline_ms = now - offline_since_ms;

	if (now >= next_soft_reset_ms) {
		LOG_WRN("Watchdog soft reset (offline %llds) — session + DNS",
			(long long)(offline_ms / 1000));
		llss_client_net_reset();
		soft_interval_ms = MIN(soft_interval_ms * 2, SOFT_MAX_MS);
		next_soft_reset_ms = now + soft_interval_ms;
	}

	if (now >= next_reboot_ms) {
		take_reset_action(offline_ms);
	}
}

void net_watchdog_ok(void)
{
	wdt_init();

	if (offline_since_ms != 0) {
		LOG_INF("Connectivity restored after %lld s offline",
			(long long)((k_uptime_get() - offline_since_ms) / 1000));
	}

	offline_since_ms = 0;
	next_soft_reset_ms = 0;
	next_reboot_ms = 0;
	soft_interval_ms = SOFT_BASE_MS;

	/* Recovered — clear the reboot escalation so a future outage starts at base. */
	rtc_reboot_backoff_s = REBOOT_BASE_S;
	rtc_wdt_magic = WDT_RTC_MAGIC;
}

#else /* !CONFIG_LLSS_WATCHDOG */

void net_watchdog_fail(void) { }
void net_watchdog_ok(void) { }

#endif /* CONFIG_LLSS_WATCHDOG */
