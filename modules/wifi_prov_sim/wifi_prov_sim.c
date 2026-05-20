/*
 * wifi_prov_sim — host-side stub for native_sim builds.
 *
 * The TAP interface is brought up by NET_CONFIG_AUTO_INIT before main()
 * runs, so "provisioning" reduces to announcing CONNECTED to the
 * application after a brief settle delay (gives the L2 stack time to
 * complete duplicate-address detection and bind the default route).
 */

#include "wifi_prov.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wifi_prov_sim, LOG_LEVEL_INF);

#define SIM_CONNECT_DELAY_MS 500
#define SIM_IP_ADDR          "192.0.2.1"
#define SIM_SSID             "native-sim"

static wifi_prov_cb_t app_cb;
static enum wifi_prov_state state = WIFI_PROV_IDLE;

static void announce_connected(struct k_work *work)
{
	ARG_UNUSED(work);
	state = WIFI_PROV_CONNECTED;
	LOG_INF("Simulated WiFi connected: ip=%s ssid=%s", SIM_IP_ADDR, SIM_SSID);
	if (app_cb) {
		app_cb(WIFI_PROV_CONNECTED, SIM_IP_ADDR);
	}
}

static K_WORK_DELAYABLE_DEFINE(connect_work, announce_connected);

void wifi_prov_init(wifi_prov_cb_t cb)
{
	app_cb = cb;
	state = WIFI_PROV_IDLE;
}

void wifi_prov_start(void)
{
	state = WIFI_PROV_CONNECTING;
	LOG_INF("Simulated WiFi connecting...");
	k_work_schedule(&connect_work, K_MSEC(SIM_CONNECT_DELAY_MS));
}

void wifi_prov_start_ap(void)
{
	LOG_WRN("wifi_prov_start_ap is a no-op on native_sim");
}

enum wifi_prov_state wifi_prov_get_state(void)
{
	return state;
}

const char *wifi_prov_get_ip(void)
{
	return (state == WIFI_PROV_CONNECTED) ? SIM_IP_ADDR : "";
}

const char *wifi_prov_get_ssid(void)
{
	return SIM_SSID;
}

void wifi_prov_clear_credentials(void)
{
	LOG_INF("wifi_prov_clear_credentials: no-op on native_sim");
}
