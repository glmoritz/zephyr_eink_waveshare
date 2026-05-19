/*
 * NTP time synchronisation using dns_get_addr_info + sntp_simple_addr.
 * No heap allocations — DNS results land in a stack-allocated struct.
 * The PCF85063ATL RTC is updated on every successful sync and is read
 * back on boot to prime CLOCK_REALTIME before the first network sync.
 */

#include "ntp_sync.h"

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

#define NTP_PORT            123
#define NTP_QUERY_TIMEOUT_MS 8000
#define MIN_VALID_UNIX_TIME 1704067200LL /* 2024-01-01 00:00:00 UTC */

static const struct device *rtc_dev = DEVICE_DT_GET(DT_NODELABEL(rtc0));

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

	/* AAAA first */
	if (dns_get_addr_info(host, DNS_QUERY_TYPE_AAAA, &dns_id,
			      dns_ntp_cb, &state,
			      CONFIG_LLSS_DNS_TIMEOUT_MS) == 0) {
		k_sem_take(&state.done, K_FOREVER);
	}

	/* Fall back to A if no IPv6 result */
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

	/* Patch in port 123 */
	if (state.addr.ss_family == AF_INET6) {
		((struct sockaddr_in6 *)&state.addr)->sin6_port =
			htons(NTP_PORT);
	} else {
		((struct sockaddr_in *)&state.addr)->sin_port =
			htons(NTP_PORT);
	}

	memcpy(addr_out, &state.addr, state.addrlen);
	*len_out = state.addrlen;
	return 0;
}

/* =========================================================================
 * Time conversion helpers
 * ========================================================================= */

/* Unix timestamp + nsec → rtc_time (UTC) using standard gmtime_r */
static int timespec_to_rtc(const struct timespec *ts, struct rtc_time *t)
{
	if (gmtime_r(&ts->tv_sec, rtc_time_to_tm(t)) == NULL) {
		return -EINVAL;
	}
	t->tm_nsec = (int32_t)ts->tv_nsec;
	return 0;
}

/* rtc_time → Unix seconds via Zephyr's timeutil_timegm() */
static int64_t rtc_to_unix(const struct rtc_time *t)
{
	/* rtc_time and struct tm are layout-compatible */
	return timeutil_timegm64((const struct tm *)t);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

bool ntp_sync_time_is_valid(void)
{
	struct timespec ts = {0};

	if (sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) != 0) {
		return false;
	}

	return ts.tv_sec >= MIN_VALID_UNIX_TIME;
}

void ntp_sync_restore_from_rtc(void)
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

	/* Reject obviously-unset RTC (year before 2024) */
	if (t.tm_year + 1900 < 2024) {
		LOG_WRN("RTC not set (year %d) — awaiting NTP sync",
			t.tm_year + 1900);
		return;
	}

	int64_t unix_sec = rtc_to_unix(&t);

	if (unix_sec < 0) {
		LOG_WRN("rtc_to_unix returned negative value");
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

int ntp_sync_once(void)
{
	struct sockaddr_storage addr;
	socklen_t addrlen;

	int rc = resolve_ntp(CONFIG_LLSS_NTP_SERVER,
			     (struct sockaddr *)&addr, &addrlen);

	if (rc) {
		LOG_ERR("NTP DNS resolve '%s' failed: %d",
			CONFIG_LLSS_NTP_SERVER, rc);
		return rc;
	}

	struct sntp_time sntp_ts;

	rc = sntp_simple_addr((struct sockaddr *)&addr, addrlen,
			      NTP_QUERY_TIMEOUT_MS, &sntp_ts);
	if (rc) {
		LOG_ERR("NTP query failed: %d", rc);
		return rc;
	}

	/* Update RTC */
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
		rc = rtc_set_time(rtc_dev, &rtc_t);
		if (rc) {
			LOG_WRN("rtc_set_time failed: %d", rc);
		}
	}

	/* Update system clock for mbedTLS cert validation */
	sys_clock_settime(SYS_CLOCK_REALTIME, &tspec);

	LOG_INF("NTP synced: %04d-%02d-%02d %02d:%02d:%02d UTC",
		rtc_t.tm_year + 1900, rtc_t.tm_mon + 1, rtc_t.tm_mday,
		rtc_t.tm_hour, rtc_t.tm_min, rtc_t.tm_sec);
	return 0;
}

/* =========================================================================
 * Periodic sync via system workqueue
 * ========================================================================= */

static void ntp_sync_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(ntp_sync_work, ntp_sync_work_handler);

static void ntp_sync_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	ntp_sync_once();

	/* Reschedule regardless of success/failure */
	k_work_reschedule(&ntp_sync_work,
			  K_SECONDS(CONFIG_LLSS_NTP_SYNC_INTERVAL_S));
}

void ntp_sync_start(void)
{
	/* First sync: short delay so WiFi/DNS are fully up */
	k_work_reschedule(&ntp_sync_work, K_SECONDS(2));
}
