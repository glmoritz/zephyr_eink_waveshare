/*
 * NTP state-machine thread.
 *
 * Self-contained: owns SYS_FLAG_TIME_VALID and the PCF85063A RTC update
 * cycle.  No consumers call into this module — they just wait on the flag.
 *
 * Lifecycle:
 *   1. On boot, try to seed CLOCK_REALTIME from the RTC battery cell.  If
 *      the RTC has a believable year (>= 2024), set TIME_VALID immediately
 *      so consumers can run while we wait for the next NTP sync.
 *   2. Loop:
 *        wait SYS_FLAG_WIFI_READY (no point trying NTP without network)
 *        do one NTP query
 *        on success: set TIME_VALID, sleep CONFIG_LLSS_NTP_SYNC_INTERVAL_S
 *        on failure: sleep 5 s and retry; do NOT clear TIME_VALID if the
 *        clock is still believable from the previous successful sync.
 *
 * Try / fail / wait.  No polling, no callbacks.
 */

#include "ntp_sync.h"
#include "system_flags.h"

#include <stdint.h>
#include <time.h>

#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/timeutil.h>

LOG_MODULE_REGISTER(ntp_sync, LOG_LEVEL_INF);

#define NTP_PORT             123
#define NTP_QUERY_TIMEOUT_MS 8000
#define MIN_VALID_UNIX_TIME  1704067200LL /* 2024-01-01 00:00:00 UTC */

#define NTP_THREAD_STACK    4096
#define NTP_THREAD_PRIORITY 11

#define NTP_RETRY_BACKOFF_MS 5000

static const struct device *rtc_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_rtc));

/* =========================================================================
 * DNS helper — heap-free, same pattern as llss_client
 * ========================================================================= */

struct dns_ntp_state {
	struct k_sem           done;
	struct sockaddr_storage addr;
	socklen_t              addrlen;
	bool                   found;
};

static void dns_ntp_cb(enum dns_resolve_status status,
		       struct dns_addrinfo *info,
		       void *user_data)
{
	struct dns_ntp_state *state = user_data;

	if (status == DNS_EAI_INPROGRESS && info && !state->found) {
		memcpy(&state->addr, &info->ai_addr, info->ai_addrlen);
		state->addrlen = info->ai_addrlen;
		state->found = true;
	}

	if (status != DNS_EAI_INPROGRESS) {
		k_sem_give(&state->done);
	}
}

static int resolve_ntp(const char *host, struct sockaddr *addr_out,
		       socklen_t *len_out)
{
	struct dns_ntp_state state = { .found = false };
	uint16_t dns_id;

	k_sem_init(&state.done, 0, 1);

	if (dns_get_addr_info(host, DNS_QUERY_TYPE_AAAA, &dns_id,
			      dns_ntp_cb, &state,
			      CONFIG_LLSS_DNS_TIMEOUT_MS) == 0) {
		k_sem_take(&state.done, K_FOREVER);
	}

	if (!state.found) {
		k_sem_init(&state.done, 0, 1);
		if (dns_get_addr_info(host, DNS_QUERY_TYPE_A, &dns_id,
				      dns_ntp_cb, &state,
				      CONFIG_LLSS_DNS_TIMEOUT_MS) == 0) {
			k_sem_take(&state.done, K_FOREVER);
		}
	}

	if (!state.found) {
		return -EHOSTUNREACH;
	}

	if (state.addr.ss_family == AF_INET6) {
		((struct sockaddr_in6 *)&state.addr)->sin6_port = htons(NTP_PORT);
	} else {
		((struct sockaddr_in *)&state.addr)->sin_port = htons(NTP_PORT);
	}

	memcpy(addr_out, &state.addr, state.addrlen);
	*len_out = state.addrlen;
	return 0;
}

/* =========================================================================
 * Time conversion helpers
 * ========================================================================= */

static int timespec_to_rtc(const struct timespec *ts, struct rtc_time *t)
{
	if (gmtime_r(&ts->tv_sec, rtc_time_to_tm(t)) == NULL) {
		return -EINVAL;
	}
	t->tm_nsec = (int32_t)ts->tv_nsec;
	return 0;
}

static int64_t rtc_to_unix(const struct rtc_time *t)
{
	return timeutil_timegm64((const struct tm *)t);
}

static bool clock_realtime_is_valid(void)
{
	struct timespec ts = {0};

	if (sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) != 0) {
		return false;
	}
	return ts.tv_sec >= MIN_VALID_UNIX_TIME;
}

/* =========================================================================
 * Restore CLOCK_REALTIME from the PCF85063A battery-backed RTC.
 * Called once at thread entry.
 * ========================================================================= */

static void restore_from_rtc(void)
{
	if (!device_is_ready(rtc_dev)) {
		LOG_WRN("RTC not ready, cannot restore time");
		return;
	}

	struct rtc_time t = {0};

	if (rtc_get_time(rtc_dev, &t) != 0) {
		LOG_WRN("rtc_get_time failed");
		return;
	}

	if (t.tm_year + 1900 < 2024) {
		LOG_WRN("RTC not set (year %d) — awaiting NTP",
			t.tm_year + 1900);
		return;
	}

	int64_t unix_sec = rtc_to_unix(&t);

	if (unix_sec < 0) {
		LOG_WRN("rtc_to_unix returned negative");
		return;
	}

	struct timespec ts = {
		.tv_sec  = (time_t)unix_sec,
		.tv_nsec = t.tm_nsec,
	};

	sys_clock_settime(SYS_CLOCK_REALTIME, &ts);

	LOG_INF("Restored CLOCK_REALTIME from RTC: "
		"%04d-%02d-%02d %02d:%02d:%02d UTC",
		t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
		t.tm_hour, t.tm_min, t.tm_sec);
}

/* =========================================================================
 * One NTP query + RTC + CLOCK_REALTIME update.  Caller is the thread.
 * ========================================================================= */

static int do_ntp_sync_once(void)
{
	struct sockaddr_storage addr;
	socklen_t addrlen;

	int32_t rc = resolve_ntp(CONFIG_LLSS_NTP_SERVER,
				 (struct sockaddr *)&addr, &addrlen);

	if (rc) {
		LOG_WRN("NTP DNS resolve '%s' failed: %d",
			CONFIG_LLSS_NTP_SERVER, rc);
		return rc;
	}

	struct sntp_time sntp_ts;

	rc = sntp_simple_addr((struct sockaddr *)&addr, addrlen,
			      NTP_QUERY_TIMEOUT_MS, &sntp_ts);
	if (rc) {
		LOG_WRN("NTP query failed: %d", rc);
		return rc;
	}

	struct rtc_time rtc_t = {0};
	struct timespec tspec = {
		.tv_sec  = (time_t)sntp_ts.seconds,
		.tv_nsec = ((uint64_t)sntp_ts.fraction * 1000000000ULL) >> 32,
	};

	if (timespec_to_rtc(&tspec, &rtc_t) != 0) {
		LOG_ERR("timespec_to_rtc failed");
		return -EINVAL;
	}

	if (device_is_ready(rtc_dev)) {
		int32_t wrc = rtc_set_time(rtc_dev, &rtc_t);

		if (wrc) {
			LOG_WRN("rtc_set_time failed: %d", wrc);
		}
	}

	sys_clock_settime(SYS_CLOCK_REALTIME, &tspec);

	LOG_INF("NTP synced: %04d-%02d-%02d %02d:%02d:%02d UTC",
		rtc_t.tm_year + 1900, rtc_t.tm_mon + 1, rtc_t.tm_mday,
		rtc_t.tm_hour, rtc_t.tm_min, rtc_t.tm_sec);
	return 0;
}

/* =========================================================================
 * NTP thread — try / fail / wait
 * ========================================================================= */

static void ntp_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	restore_from_rtc();
	if (clock_realtime_is_valid()) {
		sys_flag_set(SYS_FLAG_TIME_VALID);
	}

	while (true) {
		/* Without WiFi, NTP is impossible.  Block here — no polling. */
		sys_flag_wait_all(SYS_FLAG_WIFI_READY, K_FOREVER);

		sys_io_acquire();
		int32_t rc = do_ntp_sync_once();
		sys_io_release();

		if (rc == 0) {
			sys_flag_set(SYS_FLAG_TIME_VALID);
			k_sleep(K_SECONDS(CONFIG_LLSS_NTP_SYNC_INTERVAL_S));
		} else {
			/* Don't clear TIME_VALID — the RTC reading we restored
			 * at boot may still be good enough for cert validation.
			 * Only fresh sync failed; retry shortly. */
			k_sleep(K_MSEC(NTP_RETRY_BACKOFF_MS));
		}
	}
}

K_THREAD_DEFINE(ntp_thread, NTP_THREAD_STACK,
		ntp_thread_fn, NULL, NULL, NULL,
		NTP_THREAD_PRIORITY, 0, 0);
