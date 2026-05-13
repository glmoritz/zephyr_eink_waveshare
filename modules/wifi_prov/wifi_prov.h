#ifndef WIFI_PROV_H_
#define WIFI_PROV_H_

#include <stdbool.h>
#include <zephyr/kernel.h>

/**
 * WiFi provisioning states.
 */
enum wifi_prov_state {
	WIFI_PROV_IDLE = 0,
	WIFI_PROV_CONNECTING,
	WIFI_PROV_CONNECTED,
	WIFI_PROV_DISCONNECTED,
	WIFI_PROV_AP_ACTIVE,
};

/** State-change callback: called from system work queue. */
typedef void (*wifi_prov_cb_t)(enum wifi_prov_state state, const char *info);

void wifi_prov_init(wifi_prov_cb_t cb);
void wifi_prov_start(void);
void wifi_prov_start_ap(void);
enum wifi_prov_state wifi_prov_get_state(void);
const char *wifi_prov_get_ip(void);
const char *wifi_prov_get_ssid(void);
void wifi_prov_clear_credentials(void);

#endif /* WIFI_PROV_H_ */
