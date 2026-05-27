/*
 * LLSS e-Ink Client — boot and init only
 *
 * main() initialises the display, starts WiFi provisioning, and returns.
 * All application logic runs in two statically-defined threads:
 *   llss_thread    (llss_thread.c)   — state machine + API calls
 *   display_thread (display_thread.c) — LVGL rendering
 *
 * Board: eink_llss_esp32 (ESP32-S3 N16R8)
 * RTOS:  Zephyr 4.3.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "device_ui.h"
#include "display_thread.h"
#include "llss_thread.h"
#include "wifi_prov.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
	int rc;

	k_msleep(1500); /* USB JTAG enumeration */

	LOG_INF("=== LLSS e-Ink Client v%s ===", CONFIG_LLSS_FIRMWARE_VERSION);

	ui_init();
	ui_log_push("=== LLSS e-Ink Client v" CONFIG_LLSS_FIRMWARE_VERSION " ===");
	rc = settings_subsys_init();
	if (rc) {
		LOG_ERR("settings_subsys_init failed: %d", rc);
	} else {
		rc = settings_load_subtree("alarm");
		if (rc) {
			LOG_ERR("settings_load_subtree(alarm) failed: %d", rc);
		} else {
			device_ui_settings_ready();
		}
	}

	wifi_prov_init(llss_on_wifi);
	wifi_prov_start();

	return 0;
}
