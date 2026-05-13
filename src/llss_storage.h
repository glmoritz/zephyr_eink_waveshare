#ifndef LLSS_STORAGE_H_
#define LLSS_STORAGE_H_

#include <stddef.h>
#include <stdint.h>

#include <autoconf.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum field lengths (including NUL terminator) — defined in Kconfig */
#define LLSS_DEVICE_ID_MAX     CONFIG_LLSS_DEVICE_ID_MAX
#define LLSS_DEVICE_SECRET_MAX CONFIG_LLSS_DEVICE_SECRET_MAX
#define LLSS_TOKEN_MAX         CONFIG_LLSS_TOKEN_MAX

/**
 * @brief Initialise the Zephyr settings subsystem for LLSS storage.
 *
 * Must be called once before any other llss_storage_* function, after
 * settings_subsys_init() and settings_load() have been called by the app.
 *
 * @return 0 on success, negative errno otherwise.
 */
int llss_storage_init(void);

/**
 * @brief Load all stored credentials into the provided buffers.
 *
 * Any field not found in storage is set to an empty string.
 *
 * @param device_id      Buffer for device_id  (>= LLSS_DEVICE_ID_MAX bytes)
 * @param device_secret  Buffer for device_secret (>= LLSS_DEVICE_SECRET_MAX)
 * @param refresh_token  Buffer for refresh_token (>= LLSS_TOKEN_MAX bytes)
 * @param access_token   Buffer for access_token  (>= LLSS_TOKEN_MAX bytes)
 * @return 0 on success, negative errno otherwise.
 */
int llss_storage_load(char *device_id, char *device_secret,
		      char *refresh_token, char *access_token);

/**
 * @brief Persist device registration credentials.
 */
int llss_storage_save_device(const char *device_id,
			     const char *device_secret);

/**
 * @brief Persist token pair (refresh + access).
 */
int llss_storage_save_tokens(const char *refresh_token,
			     const char *access_token);

/**
 * @brief Persist only the access token (after a refresh).
 */
int llss_storage_save_access_token(const char *access_token);

/**
 * @brief Erase all LLSS credentials from storage.
 */
int llss_storage_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* LLSS_STORAGE_H_ */
