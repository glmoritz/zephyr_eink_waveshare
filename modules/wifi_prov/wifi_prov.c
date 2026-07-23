#include "wifi_prov.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(wifi_prov, LOG_LEVEL_DBG);

/* Log every DNS server currently registered in the default resolver context.
 * Called after DHCP so the addresses from option 6 are already populated. */
static void log_dns_servers(void)
{
	const struct dns_resolve_context *ctx = dns_resolve_get_default();

	if (!ctx) {
		LOG_WRN("DNS: no default resolver context");
		return;
	}

	char buf[NET_IPV6_ADDR_LEN];
	bool found = false;

	for (int32_t i = 0; i < ARRAY_SIZE(ctx->servers); i++) {
		const struct sockaddr *sa = &ctx->servers[i].dns_server;

		if (sa->sa_family == AF_INET || sa->sa_family == AF_INET6) {
			net_addr_ntop(sa->sa_family,
				      (sa->sa_family == AF_INET)
						? (void *)&net_sin(sa)->sin_addr
						: (void *)&net_sin6(sa)->sin6_addr,
				      buf, sizeof(buf));
			LOG_INF("DNS server [%d]: %s (source: %s)",
				i, buf,
				dns_get_source_str(ctx->servers[i].source));
			found = true;
		}
	}

	if (!found) {
		LOG_WRN("DNS: no servers configured (DHCP option 6 missing?)");
	}
}

static void ensure_dns_context_active(struct net_if *iface)
{
	struct dns_resolve_context *ctx = dns_resolve_get_default();
	struct sockaddr server_addrs[CONFIG_DNS_RESOLVER_MAX_SERVERS];
	const struct sockaddr *servers[CONFIG_DNS_RESOLVER_MAX_SERVERS + 1];
	int32_t ifaces[CONFIG_DNS_RESOLVER_MAX_SERVERS + 1];
	int32_t count = 0;
	int32_t rc;

	if (!ctx || !iface) {
		return;
	}

	if (ctx->state == DNS_RESOLVE_CONTEXT_ACTIVE) {
		return;
	}

	for (int32_t i = 0; i < ARRAY_SIZE(ctx->servers) && count < CONFIG_DNS_RESOLVER_MAX_SERVERS; i++) {
		const struct sockaddr *server = &ctx->servers[i].dns_server;

		if (server->sa_family != AF_INET && server->sa_family != AF_INET6) {
			continue;
		}

		memcpy(&server_addrs[count], server, sizeof(struct sockaddr));
		servers[count] = &server_addrs[count];
		ifaces[count] = ctx->servers[i].if_index > 0 ?
			ctx->servers[i].if_index : net_if_get_by_iface(iface);
		count++;
	}

	if (count == 0) {
		LOG_WRN("DNS: resolver inactive and no learned servers to reactivate it");
		return;
	}

	servers[count] = NULL;
	ifaces[count] = 0;

	rc = dns_resolve_close(ctx);
	if (rc < 0) {
		LOG_WRN("DNS: close before reconfigure returned %d", rc);
	}

	rc = dns_resolve_reconfigure_with_interfaces(ctx, NULL, servers, ifaces,
		DNS_SOURCE_UNKNOWN);
	if (rc < 0) {
		LOG_ERR("DNS: failed to reactivate resolver context: %d", rc);
		return;
	}

	LOG_INF("DNS: resolver context reactivated with %d server(s) on iface %d",
		count, net_if_get_by_iface(iface));
}

/* =========================================================================
 * Settings-backed credential storage
 * ========================================================================= */

static char stored_ssid[33];
static char stored_pass[65];

static int wifi_settings_set(const char *key, size_t len,
			    settings_read_cb read_cb, void *cb_arg)
{
	char *dst = NULL;
	size_t dst_len = 0;
	ssize_t r;

	if (!strcmp(key, "ssid")) {
		dst = stored_ssid;
		dst_len = sizeof(stored_ssid);
	} else if (!strcmp(key, "pass")) {
		dst = stored_pass;
		dst_len = sizeof(stored_pass);
	} else {
		return 0;
	}

	r = read_cb(cb_arg, dst, dst_len - 1);
	if (r < 0) {
		return (int32_t)r;
	}

	dst[r] = '\0';
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(wifi_prov_store, "wifi", NULL,
			       wifi_settings_set, NULL, NULL);

static int creds_init_storage(void)
{
	memset(stored_ssid, 0, sizeof(stored_ssid));
	memset(stored_pass, 0, sizeof(stored_pass));
	return settings_load_subtree("wifi");
}

static bool creds_load(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
	if (!stored_ssid[0]) {
		return false;
	}

	strncpy(ssid, stored_ssid, ssid_sz - 1);
	ssid[ssid_sz - 1] = '\0';
	strncpy(pass, stored_pass, pass_sz - 1);
	pass[pass_sz - 1] = '\0';

	return ssid[0] != '\0';
}

static void creds_save(const char *ssid, const char *pass)
{
	int32_t rc;

	rc = settings_save_one("wifi/ssid", ssid, strlen(ssid) + 1);
	if (rc) {
		LOG_ERR("save wifi/ssid: %d", rc);
		return;
	}

	rc = settings_save_one("wifi/pass", pass, strlen(pass) + 1);
	if (rc) {
		LOG_ERR("save wifi/pass: %d", rc);
		return;
	}

	strncpy(stored_ssid, ssid, sizeof(stored_ssid) - 1);
	stored_ssid[sizeof(stored_ssid) - 1] = '\0';
	strncpy(stored_pass, pass, sizeof(stored_pass) - 1);
	stored_pass[sizeof(stored_pass) - 1] = '\0';
	LOG_INF("Credentials saved for SSID: %s", ssid);
}

static void creds_erase(void)
{
	settings_delete("wifi/ssid");
	settings_delete("wifi/pass");
	stored_ssid[0] = '\0';
	stored_pass[0] = '\0';
	LOG_INF("Credentials erased");
}

/* =========================================================================
 * State management
 * ========================================================================= */

static wifi_prov_cb_t state_cb;
static enum wifi_prov_state prov_state = WIFI_PROV_IDLE;

static char ip_str[NET_IPV4_ADDR_LEN];
static char ip6_str[NET_IPV6_ADDR_LEN];
static char ssid_cur[33];

static void set_state(enum wifi_prov_state s, const char *info)
{
	prov_state = s;
	if (state_cb) {
		state_cb(s, info ? info : "");
	}
}

/* =========================================================================
 * Net management event callbacks
 * ========================================================================= */

#define NET_EVENT_WIFI_MASK                                                    \
	(NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT |    \
	 NET_EVENT_WIFI_AP_ENABLE_RESULT | NET_EVENT_WIFI_AP_STA_CONNECTED)

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static struct net_mgmt_event_callback ipv6_cb;

static K_SEM_DEFINE(sem_wifi_connected, 0, 1);
static K_SEM_DEFINE(sem_ip_ready, 0, 1);
static K_SEM_DEFINE(sem_ip6_ready, 0, 1);

static void set_default_iface(struct net_if *iface, const char *reason)
{
	if (!iface) {
		return;
	}

	if (net_if_get_default() == iface) {
		return;
	}

	net_if_set_default(iface);
	LOG_INF("Default iface -> %d (%s)",
		net_if_get_by_iface(iface), reason);
}

/* =========================================================================
 * STA auto-reconnect
 *
 * Without this, a router reboot is terminal: the disconnect event cleared
 * SYS_FLAG_WIFI_READY and tore down the TLS session, but sta_connect() was
 * only ever called from the one-shot boot/portal paths, so nothing re-issued
 * NET_REQUEST_WIFI_CONNECT. The LLSS thread then blocked forever on
 * SYS_FLAG_WIFI_READY while the UI promised "reconectando...".
 *
 * Deliberately retries forever and never falls back to AP mode: a device that
 * opens a provisioning AP because the WiFi blipped is worse than one that keeps
 * trying. AP fallback stays where it belongs — first boot with no/bad creds.
 *
 * Runs on its own thread rather than the system workqueue because sta_connect()
 * blocks for up to ~60 s (association + DHCP/SLAAC waits), and it reuses
 * sta_connect() wholesale so the state transitions, default-iface selection and
 * IP waits stay in exactly one place.
 * ========================================================================= */

#define RECONNECT_BASE_S 2U
#define RECONNECT_MAX_S  60U

/* Credentials cached from the last sta_connect() so the reconnect thread never
 * has to touch NVS (flash reads need an internal-DRAM destination — see memory
 * note project_nvs_rtc_buffer_nomem). .bss, not PSRAM. */
static char sta_ssid[33];
static char sta_pass[65];
static bool sta_creds_valid;

static atomic_t sta_link_up = ATOMIC_INIT(0);
static K_SEM_DEFINE(sem_reconnect, 0, 1);

/* sta_connect() drives shared state (ssid_cur, ip_str, the association/IP
 * semaphores), so the boot path and the reconnect thread must not run it
 * concurrently — a disconnect arriving during the boot connect would otherwise
 * have both in flight at once. */
static K_MUTEX_DEFINE(sta_connect_mutex);

static int sta_connect(const char *ssid, const char *pass);

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
				uint64_t mgmt_event, struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		const struct wifi_status *st =
			(const struct wifi_status *)cb->info;
		if (st && st->status == 0) {
			LOG_INF("WiFi STA connected: %s", ssid_cur);
			atomic_set(&sta_link_up, 1);
		} else {
			LOG_WRN("WiFi connection failed: %d",
				st ? st->status : -1);
			atomic_set(&sta_link_up, 0);
		}
		k_sem_give(&sem_wifi_connected);
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		LOG_WRN("WiFi STA disconnected");
		atomic_set(&sta_link_up, 0);
		set_state(WIFI_PROV_DISCONNECTED, ssid_cur);
		/* Kick the reconnect thread. Only meaningful once we've had a
		 * successful STA connect (creds cached) and while we're not
		 * serving the provisioning AP — reconnecting under the portal
		 * would fight the user configuring it. */
		if (sta_creds_valid && prov_state != WIFI_PROV_AP_ACTIVE) {
			k_sem_give(&sem_reconnect);
		}
		break;
	case NET_EVENT_WIFI_AP_ENABLE_RESULT:
		LOG_INF("WiFi AP enabled: %s", ssid_cur);
		set_state(WIFI_PROV_AP_ACTIVE, ssid_cur);
		break;
	case NET_EVENT_WIFI_AP_STA_CONNECTED:
		LOG_INF("Client connected to AP");
		break;
	default:
		break;
	}
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb,
				uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);

	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}

	struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;

	if (ipv4 && ipv4->unicast[0].ipv4.is_used) {
		set_default_iface(iface, "IPv4 address ready");
		net_addr_ntop(AF_INET,
			      &ipv4->unicast[0].ipv4.address.in_addr,
			      ip_str, sizeof(ip_str));
		LOG_INF("Got IPv4: %s", ip_str);
		ensure_dns_context_active(iface);
		k_sem_give(&sem_ip_ready);
	}
}

static void ipv6_event_handler(struct net_mgmt_event_callback *cb,
				uint64_t mgmt_event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(mgmt_event); /* only NET_EVENT_IPV6_ADDR_ADD registered */

	struct net_if_ipv6 *ipv6 = iface->config.ip.ipv6;

	if (!ipv6) {
		return;
	}

	/* Walk unicast addresses looking for a preferred global address */
	for (int32_t i = 0; i < NET_IF_MAX_IPV6_ADDR; i++) {
		if (!ipv6->unicast[i].is_used) {
			continue;
		}
		const struct in6_addr *a =
			&ipv6->unicast[i].address.in6_addr;

		if (!net_ipv6_is_global_addr(a)) {
			continue; /* skip link-local / loopback */
		}

		set_default_iface(iface, "IPv6 global address ready");
		net_addr_ntop(AF_INET6, a, ip6_str, sizeof(ip6_str));
		LOG_INF("Got IPv6 (SLAAC): %s", ip6_str);

		k_sem_give(&sem_ip6_ready);

		/* Re-fire CONNECTED so the UI updates when IPv6 arrives late */
		if (prov_state == WIFI_PROV_CONNECTED) {
			char combined[NET_IPV4_ADDR_LEN + 2 + NET_IPV6_ADDR_LEN];

			snprintf(combined, sizeof(combined),
				 "%s / %s", ip_str, ip6_str);
			set_state(WIFI_PROV_CONNECTED, combined);
		}
		return;
	}
}

/* =========================================================================
 * STA connection
 * ========================================================================= */

static int sta_connect(const char *ssid, const char *pass)
{
	struct net_if *iface = net_if_get_wifi_sta();

	if (!iface) {
		iface = net_if_get_default();
	}

	set_default_iface(iface, "STA connect start");

	strncpy(ssid_cur, ssid, sizeof(ssid_cur) - 1);
	set_state(WIFI_PROV_CONNECTING, ssid);

	/* Cache for the reconnect thread (see STA auto-reconnect above). */
	strncpy(sta_ssid, ssid, sizeof(sta_ssid) - 1);
	sta_ssid[sizeof(sta_ssid) - 1] = '\0';
	strncpy(sta_pass, pass ? pass : "", sizeof(sta_pass) - 1);
	sta_pass[sizeof(sta_pass) - 1] = '\0';
	sta_creds_valid = true;

	k_sem_reset(&sem_wifi_connected);
	k_sem_reset(&sem_ip_ready);
	k_sem_reset(&sem_ip6_ready);
	ip_str[0]  = '\0';
	ip6_str[0] = '\0';

	struct wifi_connect_req_params params = {
		.ssid = (const uint8_t *)ssid,
		.ssid_length = strlen(ssid),
		.security = (pass && strlen(pass) > 0)
				    ? WIFI_SECURITY_TYPE_PSK
				    : WIFI_SECURITY_TYPE_NONE,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
		.mfp = WIFI_MFP_OPTIONAL,
	};

	if (params.security == WIFI_SECURITY_TYPE_PSK) {
		params.psk = (const uint8_t *)pass;
		params.psk_length = strlen(pass);
	}

	int32_t ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params,
			   sizeof(params));
	if (ret) {
		LOG_ERR("NET_REQUEST_WIFI_CONNECT: %d", ret);
		return ret;
	}

	if (k_sem_take(&sem_wifi_connected, K_SECONDS(30)) != 0) {
		LOG_WRN("WiFi association timeout");
		return -ETIMEDOUT;
	}

	/* Wait until we have *at least one* family of address — IPv4 via DHCP
	 * or IPv6 via SLAAC, whichever arrives first.  Network might be
	 * IPv4-only, IPv6-only, or dual-stack; we don't know in advance and
	 * we don't want to insist on the missing one.  After the first family
	 * is up, we give the second a short grace window but proceed either
	 * way. */
	struct k_poll_event ip_events[2];

	k_poll_event_init(&ip_events[0], K_POLL_TYPE_SEM_AVAILABLE,
			  K_POLL_MODE_NOTIFY_ONLY, &sem_ip_ready);
	k_poll_event_init(&ip_events[1], K_POLL_TYPE_SEM_AVAILABLE,
			  K_POLL_MODE_NOTIFY_ONLY, &sem_ip6_ready);

	LOG_INF("Waiting for first IP (v4 or v6) up to 25 s...");
	if (k_poll(ip_events, ARRAY_SIZE(ip_events), K_SECONDS(25)) < 0) {
		LOG_WRN("No IP after 25 s — aborting");
		return -ETIMEDOUT;
	}

	bool got_v4 = (ip_events[0].state == K_POLL_STATE_SEM_AVAILABLE);
	bool got_v6 = (ip_events[1].state == K_POLL_STATE_SEM_AVAILABLE);

	if (got_v4) {
		k_sem_take(&sem_ip_ready, K_NO_WAIT);
	}
	if (got_v6) {
		k_sem_take(&sem_ip6_ready, K_NO_WAIT);
	}

	LOG_INF("Got %s — waiting up to 5 s for the other family",
		got_v4 ? (got_v6 ? "v4+v6" : "v4") : "v6");

	/* Bonus wait for the family that didn't fire yet.  Harmless if
	 * already taken; just no-op-blocks for up to 5 s otherwise. */
	if (!got_v4) {
		k_sem_take(&sem_ip_ready, K_SECONDS(5));
	}
	if (!got_v6) {
		k_sem_take(&sem_ip6_ready, K_SECONDS(5));
	}

	log_dns_servers();

	/* Build status string from whichever address strings are populated. */
	char combined[NET_IPV4_ADDR_LEN + 3 + NET_IPV6_ADDR_LEN];

	if (ip_str[0] && ip6_str[0]) {
		snprintf(combined, sizeof(combined), "%s / %s", ip_str, ip6_str);
	} else if (ip_str[0]) {
		snprintf(combined, sizeof(combined), "%s", ip_str);
	} else if (ip6_str[0]) {
		snprintf(combined, sizeof(combined), "%s", ip6_str);
	} else {
		/* Defensive — k_poll said something fired but neither address
		 * string is populated.  Could happen if the event handler set
		 * a sem before writing the string; treat as failure. */
		LOG_WRN("IP event fired but no address string set");
		return -ENONET;
	}

	set_state(WIFI_PROV_CONNECTED, combined);
	return 0;
}

/* Retry until the link is back. Never gives up, never falls back to AP.
 * Backoff caps at RECONNECT_MAX_S so a long outage (router down overnight)
 * costs one association attempt a minute instead of a busy loop. */
static void wifi_reconnect_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	while (true) {
		k_sem_take(&sem_reconnect, K_FOREVER);

		uint32_t backoff_s = RECONNECT_BASE_S;

		while (!atomic_get(&sta_link_up)) {
			LOG_WRN("WiFi down — retrying %s in %u s",
				sta_ssid, backoff_s);
			k_msleep(backoff_s * 1000U);

			/* The link may have returned on its own while we slept
			 * (driver-level roam), in which case don't disturb it. */
			if (atomic_get(&sta_link_up)) {
				break;
			}

			/* If the boot path fell back to the provisioning AP while
			 * we were waiting, stand down — the user is configuring. */
			if (prov_state == WIFI_PROV_AP_ACTIVE) {
				LOG_INF("AP mode active — reconnect standing down");
				break;
			}

			k_mutex_lock(&sta_connect_mutex, K_FOREVER);
			int rc = sta_connect(sta_ssid, sta_pass);
			k_mutex_unlock(&sta_connect_mutex);

			if (rc == 0) {
				LOG_INF("WiFi reconnected to %s", sta_ssid);
				break;
			}

			backoff_s = MIN(backoff_s * 2U, RECONNECT_MAX_S);
		}
	}
}

K_THREAD_DEFINE(wifi_reconnect_tid, 3072, wifi_reconnect_thread,
		NULL, NULL, NULL, 11, 0, 0);

/* =========================================================================
 * AP mode + DHCP server
 * ========================================================================= */

#define AP_IP_STR   "192.168.4.1"
#define AP_MASK_STR "255.255.255.0"
#define AP_POOL_STR "192.168.4.11"

static int ap_enable(const char *ssid)
{
	struct net_if *iface = net_if_get_wifi_sap();

	if (!iface) {
		iface = net_if_get_default();
	}

	struct in_addr addr, mask, pool;

	net_addr_pton(AF_INET, AP_IP_STR, &addr);
	net_addr_pton(AF_INET, AP_MASK_STR, &mask);
	net_addr_pton(AF_INET, AP_POOL_STR, &pool);

	net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
	net_if_ipv4_set_gw(iface, &addr);
	net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);

	strncpy(ssid_cur, ssid, sizeof(ssid_cur) - 1);

	struct wifi_connect_req_params ap_params = {
		.ssid = (const uint8_t *)ssid,
		.ssid_length = strlen(ssid),
		.security = WIFI_SECURITY_TYPE_NONE,
		.channel = 1,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
	};

	int32_t ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &ap_params,
			   sizeof(ap_params));
	if (ret) {
		LOG_ERR("NET_REQUEST_WIFI_AP_ENABLE: %d", ret);
		return ret;
	}

	set_default_iface(iface, "AP provisioning start");
	net_dhcpv4_server_start(iface, &pool);
	return 0;
}

static void ap_disable(void)
{
	struct net_if *iface = net_if_get_wifi_sap();

	if (!iface) {
		iface = net_if_get_default();
	}

	net_dhcpv4_server_stop(iface);
	net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, iface, NULL, 0);

	/* Drop the manually-bound 192.168.4.1 so a later ap_enable() can
	 * net_if_ipv4_addr_add() it again without a duplicate-add failure
	 * that leaves the SAP iface without a usable IPv4 stack.  Silently
	 * tolerate "not there" — we may be called from a state where the
	 * binding was already removed (or never existed). */
	struct in_addr addr;

	if (net_addr_pton(AF_INET, AP_IP_STR, &addr) == 0) {
		(void)net_if_ipv4_addr_rm(iface, &addr);
	}

	k_msleep(500);
}

/* =========================================================================
 * Captive portal HTTP server (raw TCP, port 80)
 * ========================================================================= */

static const char FORM_RESPONSE[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: text/html\r\n"
	"Connection: close\r\n"
	"\r\n"
	"<!DOCTYPE html><html><head>"
	"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
	"<title>LLSS WiFi Setup</title>"
	"<style>"
	"body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 20px;background:#f5f5f5}"
	"h1{color:#333}"
	"input{width:100%;padding:10px;margin:6px 0;box-sizing:border-box;"
	"border:1px solid #ccc;border-radius:4px}"
	"button{width:100%;padding:12px;background:#2196F3;color:#fff;"
	"border:none;border-radius:4px;font-size:1em;cursor:pointer}"
	"</style></head><body>"
	"<h1>LLSS WiFi Setup</h1>"
	"<p>Connect to your WiFi network to continue setup.</p>"
	"<form method=\"POST\" action=\"/wifi\">"
	"<label>Network (SSID):</label>"
	"<input name=\"ssid\" required maxlength=\"32\" autocomplete=\"off\">"
	"<label>Password:</label>"
	"<input name=\"password\" type=\"password\" maxlength=\"64\">"
	"<button type=\"submit\">Save &amp; Connect</button>"
	"</form></body></html>\r\n";

static const char OK_RESPONSE[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: text/html\r\n"
	"Connection: close\r\n"
	"\r\n"
	"<!DOCTYPE html><html><body>"
	"<h1>Saved!</h1>"
	"<p>Connecting to your network. This hotspot will close shortly.</p>"
	"</body></html>\r\n";

static const char REDIRECT_RESPONSE[] =
	"HTTP/1.1 302 Found\r\n"
	"Location: http://192.168.4.1/\r\n"
	"Connection: close\r\n"
	"\r\n";

static volatile bool portal_got_creds;
static char portal_ssid[33];
static char portal_pass[65];

static void url_decode(char *s)
{
	char *d = s;

	while (*s) {
		if (*s == '+') {
			*d++ = ' ';
			s++;
		} else if (*s == '%' && isxdigit((uint8_t)s[1]) &&
			   isxdigit((uint8_t)s[2])) {
			char hex[3] = {s[1], s[2], '\0'};
			*d++ = (char)strtol(hex, NULL, 16);
			s += 3;
		} else {
			*d++ = *s++;
		}
	}
	*d = '\0';
}

static void parse_form(char *body, char *ssid_out, size_t ssid_sz,
		       char *pass_out, size_t pass_sz)
{
	ssid_out[0] = '\0';
	pass_out[0] = '\0';

	char *tok = strtok(body, "&");

	while (tok) {
		if (strncmp(tok, "ssid=", 5) == 0) {
			strncpy(ssid_out, tok + 5, ssid_sz - 1);
			ssid_out[ssid_sz - 1] = '\0';
		} else if (strncmp(tok, "password=", 9) == 0) {
			strncpy(pass_out, tok + 9, pass_sz - 1);
			pass_out[pass_sz - 1] = '\0';
		}
		tok = strtok(NULL, "&");
	}

	url_decode(ssid_out);
	url_decode(pass_out);
}

#define PORTAL_STACK_SIZE 4096
#define PORTAL_PRIORITY   5
#define HTTP_BUF_SIZE     1024

static K_THREAD_STACK_DEFINE(portal_stack, PORTAL_STACK_SIZE);
static struct k_thread portal_thread_data;
static volatile bool portal_running;
static char http_buf[HTTP_BUF_SIZE];

static void captive_portal_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int32_t srv, client;
	struct sockaddr_in srv_addr;

	srv = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (srv < 0) {
		LOG_ERR("socket() failed: %d", errno);
		return;
	}

	int32_t opt = 1;
	zsock_setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&srv_addr, 0, sizeof(srv_addr));
	srv_addr.sin_family = AF_INET;
	srv_addr.sin_port = htons(80);
	srv_addr.sin_addr.s_addr = INADDR_ANY;

	if (zsock_bind(srv, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
		LOG_ERR("bind() failed: %d", errno);
		zsock_close(srv);
		return;
	}

	zsock_listen(srv, 3);
	LOG_INF("Captive portal listening on :80");

	while (portal_running) {
		struct zsock_timeval tv = {.tv_sec = 1, .tv_usec = 0};
		zsock_setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		client = zsock_accept(srv, (struct sockaddr *)&client_addr,
				      &client_len);
		if (client < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				LOG_ERR("accept() err: %d", errno);
			}
			continue;
		}

		tv.tv_sec = 3;
		zsock_setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		ssize_t len = zsock_recv(client, http_buf, sizeof(http_buf) - 1, 0);
		if (len <= 0) {
			zsock_close(client);
			continue;
		}
		http_buf[len] = '\0';

		bool is_post_wifi = (strstr(http_buf, "POST /wifi") != NULL);
		bool is_get_root = (strncmp(http_buf, "GET / ", 6) == 0) ||
				   (strncmp(http_buf, "GET /\r", 6) == 0);

		if (is_post_wifi) {
			char *body = strstr(http_buf, "\r\n\r\n");

			if (body) {
				body += 4;
				char ssid[33], pass[65];
				parse_form(body, ssid, sizeof(ssid),
					   pass, sizeof(pass));

				if (strlen(ssid) > 0) {
					zsock_send(client, OK_RESPONSE,
						   strlen(OK_RESPONSE), 0);
					zsock_close(client);
					creds_save(ssid, pass);
					strncpy(portal_ssid, ssid,
						sizeof(portal_ssid) - 1);
					strncpy(portal_pass, pass,
						sizeof(portal_pass) - 1);
					portal_got_creds = true;
					portal_running = false;
					zsock_close(srv);
					return;
				}
			}
			zsock_send(client, FORM_RESPONSE,
				   strlen(FORM_RESPONSE), 0);
		} else if (is_get_root) {
			zsock_send(client, FORM_RESPONSE,
				   strlen(FORM_RESPONSE), 0);
		} else {
			zsock_send(client, REDIRECT_RESPONSE,
				   strlen(REDIRECT_RESPONSE), 0);
		}

		zsock_close(client);
	}

	zsock_close(srv);
}

static bool portal_thread_started;

static void portal_start(void)
{
	/* If we've spun up the portal thread before, the previous instance
	 * will have set portal_running=false and returned by the time we get
	 * here.  Wait for it to fully exit before reusing the same
	 * portal_thread_data / portal_stack — k_thread_create on a struct that
	 * isn't yet in DEAD state is undefined behaviour. */
	if (portal_thread_started) {
		k_thread_join(&portal_thread_data, K_FOREVER);
	}

	portal_running = true;
	portal_got_creds = false;

	k_thread_create(&portal_thread_data, portal_stack, PORTAL_STACK_SIZE,
			captive_portal_thread, NULL, NULL, NULL,
			PORTAL_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&portal_thread_data, "captive_portal");
	portal_thread_started = true;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

static char ap_ssid_buf[33];

void wifi_prov_init(wifi_prov_cb_t cb)
{
	state_cb = cb;
	if (creds_init_storage() != 0) {
		LOG_WRN("Failed to load stored WiFi credentials");
	}

	net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
				     NET_EVENT_WIFI_MASK);
	net_mgmt_add_event_callback(&wifi_cb);

	net_mgmt_init_event_callback(&ipv4_cb, ipv4_event_handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	net_mgmt_init_event_callback(&ipv6_cb, ipv6_event_handler,
				     NET_EVENT_IPV6_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv6_cb);
}

void wifi_prov_start(void)
{
	char ssid[33], pass[65];

	if (creds_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
		LOG_INF("Stored creds SSID=%s, trying STA...", ssid);

		k_mutex_lock(&sta_connect_mutex, K_FOREVER);
		int rc = sta_connect(ssid, pass);
		k_mutex_unlock(&sta_connect_mutex);

		if (rc == 0) {
			return;
		}
		LOG_WRN("STA failed, falling back to AP");
	} else {
		LOG_INF("No stored creds — entering AP config mode");
	}

	wifi_prov_start_ap();
}

void wifi_prov_start_ap(void)
{
	struct net_if *iface = net_if_get_wifi_sap();

	if (!iface) {
		iface = net_if_get_default();
	}

	struct net_linkaddr *ll = net_if_get_link_addr(iface);

	snprintf(ap_ssid_buf, sizeof(ap_ssid_buf), "LLSS-%02X%02X%02X",
		 ll->addr[3], ll->addr[4], ll->addr[5]);

	/* Loop instead of recursing.  Each previous design failure-retry path
	 * called wifi_prov_start_ap() recursively, leaking a caller stack
	 * frame per retry — eventually blows the calling thread's stack. */
	while (true) {
		LOG_INF("Starting AP: %s", ap_ssid_buf);

		if (ap_enable(ap_ssid_buf) != 0) {
			LOG_ERR("ap_enable failed — retrying in 3s");
			k_msleep(3000);
			continue;
		}

		portal_start();

		while (!portal_got_creds) {
			k_msleep(200);
		}

		ap_disable();

		if (sta_connect(portal_ssid, portal_pass) == 0) {
			return;
		}

		LOG_WRN("STA failed after provisioning — restart AP");
		/* loop back to ap_enable */
	}
}

enum wifi_prov_state wifi_prov_get_state(void) { return prov_state; }
const char *wifi_prov_get_ip(void)             { return ip_str; }
const char *wifi_prov_get_ssid(void)           { return ssid_cur; }
void wifi_prov_clear_credentials(void)         { creds_erase(); }
