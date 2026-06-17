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

/* Reserved for the future deep-sleep resume fast path: a cold-boot-safe magic
 * in RTC slow RAM. When deep sleep lands, the wake path can stash the
 * credentials in an RTC mirror guarded by this magic and skip the NVS read.
 * Kept (unused for now) so that work doesn't start from scratch. */
#define LLSS_STORAGE_RTC_MAGIC  0x4C535343U  /* 'LSSC' */
static uint32_t rtc_cache_magic LLSS_RTC_NOINIT;

/* In-memory cache of the credentials stored in NVS, rebuilt on every boot.
 *
 * These MUST live in internal DRAM (plain .bss), NOT RTC slow RAM or PSRAM.
 * On the ESP32-S3 (non-MCUboot build) the flash driver passes the caller's
 * buffer straight to IDF esp_flash_read()/esp_flash_write(), which require a
 * destination in internal DRAM (esp_ptr_in_dram()); Zephyr provides no
 * bounce-buffer hook. A buffer in RTC RAM (0x5000_0000) or PSRAM fails that
 * check and the flash op returns ESP_ERR_NO_MEM (-EIO) — so NVS could neither
 * load the credentials at boot nor save them during the session, and the
 * device had to be re-registered after every reboot.
 *
 * DEEP-SLEEP NOTE: do NOT move these back into .rtc_noinit. The resume path
 * must keep flash I/O pointed at these DRAM buffers and copy to/from a
 * separate RTC mirror (rtc_cache_magic above), never read/write NVS straight
 * into RTC RAM, or this ESP_ERR_NO_MEM bug returns. */
static char  cache_device_id[LLSS_DEVICE_ID_MAX];
static char  cache_device_secret[LLSS_DEVICE_SECRET_MAX];
static char  cache_refresh_token[LLSS_TOKEN_MAX];
static char  cache_access_token[LLSS_TOKEN_MAX];

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
	/* Rebuild the in-memory cache from NVS on every boot. The RTC fast path
	 * is not wired up yet (reserved for deep sleep); invalidate its magic. */
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
