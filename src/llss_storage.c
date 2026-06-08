#include "llss_storage.h"

#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "section_attrs.h"

LOG_MODULE_REGISTER(llss_storage, LOG_LEVEL_DBG);

/* =========================================================================
 * Settings namespace: "llss"
 *
 * Keys:
 *   llss/device_id
 *   llss/device_secret
 *   llss/refresh_token
 *   llss/access_token
 * ========================================================================= */

#define KEY_DEVICE_ID      "llss/device_id"
#define KEY_DEVICE_SECRET  "llss/device_secret"
#define KEY_REFRESH_TOKEN  "llss/refresh_token"
#define KEY_ACCESS_TOKEN   "llss/access_token"

/* RTC slow RAM — survives deep sleep, cleared on cold boot / power loss.
 * If rtc_cache_magic == LLSS_STORAGE_RTC_MAGIC the buffers below hold valid
 * credentials from the previous wake cycle and llss_storage_init() skips
 * the Settings/NVS flash read entirely. */
#define LLSS_STORAGE_RTC_MAGIC  0x4C535343U  /* 'LSSC' */

static uint32_t rtc_cache_magic                       LLSS_RTC_NOINIT;
static char  cache_device_id[LLSS_DEVICE_ID_MAX]      LLSS_RTC_NOINIT;
static char  cache_device_secret[LLSS_DEVICE_SECRET_MAX] LLSS_RTC_NOINIT;
static char  cache_refresh_token[LLSS_TOKEN_MAX]      LLSS_RTC_NOINIT;
static char  cache_access_token[LLSS_TOKEN_MAX]       LLSS_RTC_NOINIT;

/* =========================================================================
 * settings_handler
 * ========================================================================= */

static int llss_settings_set(const char *name, size_t len,
			      settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "device_id") == 0) {
		ssize_t r = read_cb(cb_arg, cache_device_id,
				    sizeof(cache_device_id) - 1);
		if (r >= 0) {
			cache_device_id[r] = '\0';
		}
	} else if (strcmp(name, "device_secret") == 0) {
		ssize_t r = read_cb(cb_arg, cache_device_secret,
				    sizeof(cache_device_secret) - 1);
		if (r >= 0) {
			cache_device_secret[r] = '\0';
		}
	} else if (strcmp(name, "refresh_token") == 0) {
		ssize_t r = read_cb(cb_arg, cache_refresh_token,
				    sizeof(cache_refresh_token) - 1);
		if (r >= 0) {
			cache_refresh_token[r] = '\0';
		}
	} else if (strcmp(name, "access_token") == 0) {
		ssize_t r = read_cb(cb_arg, cache_access_token,
				    sizeof(cache_access_token) - 1);
		if (r >= 0) {
			cache_access_token[r] = '\0';
		}
	}

	return 0;
}

static struct settings_handler llss_handler = {
	.name = "llss",
	.h_set = llss_settings_set,
};

/* =========================================================================
 * Public API
 * ========================================================================= */

int llss_storage_init(void)
{
	/* RTC-RAM fast path is disabled until we have a cold-boot-safe magic.
	 * .rtc_noinit survives deep sleep but is uninitialised on cold boot,
	 * so the magic word can randomly match LLSS_STORAGE_RTC_MAGIC and the
	 * code below would trust uninitialised RAM as valid credentials.
	 * Always re-read from NVS; rebuild the in-memory cache from scratch. */
	memset(cache_device_id,     0, sizeof(cache_device_id));
	memset(cache_device_secret, 0, sizeof(cache_device_secret));
	memset(cache_refresh_token, 0, sizeof(cache_refresh_token));
	memset(cache_access_token,  0, sizeof(cache_access_token));
	rtc_cache_magic = 0;

	int32_t rc = settings_register(&llss_handler);

	if (rc && rc != -EEXIST) {
		LOG_ERR("settings_register: %d", rc);
		return rc;
	}

	rc = settings_load_subtree("llss");
	if (rc) {
		LOG_ERR("settings_load_subtree: %d", rc);
		return rc;
	}

	LOG_INF("LLSS storage loaded from NVS. device_id=%s",
		cache_device_id[0] ? cache_device_id : "(none)");
	return 0;
}

int llss_storage_load(char *device_id, char *device_secret,
		      char *refresh_token, char *access_token)
{
	strncpy(device_id,     cache_device_id,     LLSS_DEVICE_ID_MAX - 1);
	strncpy(device_secret, cache_device_secret, LLSS_DEVICE_SECRET_MAX - 1);
	strncpy(refresh_token, cache_refresh_token, LLSS_TOKEN_MAX - 1);
	strncpy(access_token,  cache_access_token,  LLSS_TOKEN_MAX - 1);
	device_id[LLSS_DEVICE_ID_MAX - 1]     = '\0';
	device_secret[LLSS_DEVICE_SECRET_MAX - 1] = '\0';
	refresh_token[LLSS_TOKEN_MAX - 1]     = '\0';
	access_token[LLSS_TOKEN_MAX - 1]      = '\0';
	return 0;
}

int llss_storage_save_device(const char *device_id,
			     const char *device_secret)
{
	int32_t rc;

	rc = settings_save_one(KEY_DEVICE_ID, device_id, strlen(device_id));
	if (rc) {
		LOG_ERR("save device_id: %d", rc);
		return rc;
	}

	rc = settings_save_one(KEY_DEVICE_SECRET, device_secret,
			       strlen(device_secret));
	if (rc) {
		LOG_ERR("save device_secret: %d", rc);
		return rc;
	}

	strncpy(cache_device_id, device_id,
		sizeof(cache_device_id) - 1);
	strncpy(cache_device_secret, device_secret,
		sizeof(cache_device_secret) - 1);
	return 0;
}

int llss_storage_save_tokens(const char *refresh_token,
			     const char *access_token)
{
	int32_t rc;

	rc = settings_save_one(KEY_REFRESH_TOKEN, refresh_token,
			       strlen(refresh_token));
	if (rc) {
		LOG_ERR("save refresh_token: %d", rc);
		return rc;
	}

	rc = settings_save_one(KEY_ACCESS_TOKEN, access_token,
			       strlen(access_token));
	if (rc) {
		LOG_ERR("save access_token: %d", rc);
		return rc;
	}

	strncpy(cache_refresh_token, refresh_token,
		sizeof(cache_refresh_token) - 1);
	strncpy(cache_access_token, access_token,
		sizeof(cache_access_token) - 1);
	return 0;
}

int llss_storage_save_access_token(const char *access_token)
{
	int32_t rc = settings_save_one(KEY_ACCESS_TOKEN, access_token,
				   strlen(access_token));

	if (rc) {
		LOG_ERR("save access_token: %d", rc);
		return rc;
	}

	strncpy(cache_access_token, access_token,
		sizeof(cache_access_token) - 1);
	return 0;
}

int llss_storage_clear(void)
{
	settings_delete(KEY_DEVICE_ID);
	settings_delete(KEY_DEVICE_SECRET);
	settings_delete(KEY_REFRESH_TOKEN);
	settings_delete(KEY_ACCESS_TOKEN);

	memset(cache_device_id,     0, sizeof(cache_device_id));
	memset(cache_device_secret, 0, sizeof(cache_device_secret));
	memset(cache_refresh_token, 0, sizeof(cache_refresh_token));
	memset(cache_access_token,  0, sizeof(cache_access_token));
	rtc_cache_magic = 0;

	LOG_INF("LLSS credentials cleared");
	return 0;
}
