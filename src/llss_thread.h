#ifndef LLSS_THREAD_H_
#define LLSS_THREAD_H_

#include "wifi_prov.h"

/**
 * @brief WiFi event callback — pass this directly to wifi_prov_init().
 *
 * Handles all WiFi state transitions: updates the UI, signals the LLSS
 * thread when the network comes up, and tears down the TLS session on
 * disconnect.
 */
void llss_on_wifi(enum wifi_prov_state state, const char *info);

#endif /* LLSS_THREAD_H_ */
