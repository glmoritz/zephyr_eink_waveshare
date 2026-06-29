#include "llss_client.h"
#include "llss_storage.h"
#include "certs/ca_cert.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/client.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/sys/clock.h>

LOG_MODULE_REGISTER(llss_client, LOG_LEVEL_INF);

/* =========================================================================
 * Compile-time constants derived from Kconfig
 * ========================================================================= */

/* Parse the server URL "https://host:port/base" into parts at init time. */
#define LLSS_MAX_HOSTNAME  CONFIG_LLSS_MAX_HOSTNAME
#define LLSS_MAX_BASE_PATH CONFIG_LLSS_MAX_BASE_PATH
#define LLSS_MAX_URL_LEN   CONFIG_LLSS_MAX_URL_LEN

/* General-purpose JSON receive buffer (2 KB — enough for all text responses) */
#define JSON_BUF_SIZE 2048
static uint8_t json_recv_buf[JSON_BUF_SIZE];

/* Scratch buffer the HTTP client uses to parse the raw response (status line +
 * headers + body fragments). MUST be distinct from the caller's body buffer:
 * the parser hands out body_frag pointers into this buffer while the response
 * callback copies them into the caller's buffer, so sharing one buffer aliases
 * the parse window with the accumulator and corrupts multi-fragment bodies
 * (and overruns it, since raw HTTP = headers + body > body alone).
 * 2 KB easily holds the response headers; the body streams through in
 * fragments. Single-threaded request path (llss_thread), so a shared static is
 * fine, matching json_recv_buf. */
#define HTTP_RECV_SCRATCH CONFIG_LLSS_HTTP_RECV_SCRATCH_SIZE
static uint8_t http_recv_scratch[HTTP_RECV_SCRATCH];

/* =========================================================================
 * Server connection info parsed from CONFIG_LLSS_SERVER_URL
 * ========================================================================= */

static char server_host[LLSS_MAX_HOSTNAME];
static char server_base_path[LLSS_MAX_BASE_PATH]; /* e.g. "/api" */
static int32_t  server_port;

static bool client_initialised;
static struct sockaddr_storage cached_server_addr;
static socklen_t cached_server_addrlen;
static bool cached_server_addr_valid;

/*
 * Phase A — Persistent session socket.
 * When >= 0, do_request() reuses this connection instead of opening a new
 * one.  A single TLS handshake then covers all API calls in one wake cycle.
 * Set by llss_session_open(), cleared by llss_session_close().
 */
static int32_t session_sock = -1;

/*
 * Phase B — TLS session ticket in RTC RAM.
 * The ESP32-S3 RTC slow RAM (8 KB at 0x50000000) survives deep sleep.
 * We serialise the mbedTLS session here so that after the radio wakes up
 * and Phase A opens a fresh TCP connection, the TLS abbreviated-handshake
 * (~150 ms) replaces a full handshake (~3 s).
 *
 * NOTE: Zephyr 4.3 does not expose the mbedtls_ssl_context through the
 * IPPROTO_TLS_1_2 socket API.  The in-memory session cache
 * (CONFIG_NET_SOCKETS_TLS_MAX_CLIENT_SESSION_COUNT=1) is therefore the
 * mechanism used for now; it accelerates *reconnects within the same boot*
 * (e.g. if Phase A's socket drops and must be re-opened).  Full cross-sleep
 * persistence requires either a custom TCP+mbedTLS connect path or a future
 * Zephyr API addition — see protocol_improvements.md § Phase B.2.
 */
#define LLSS_TLS_SESSION_BUF_SIZE 512
#define LLSS_TLS_SESSION_MAGIC    0x4C535354U  /* 'LSST' */
/* __attribute__((used)) prevents --gc-sections from discarding these
 * unreferenced-yet variables; without it rtc_slow_seg shows 0 bytes used. */
static uint8_t  rtc_tls_session_buf[LLSS_TLS_SESSION_BUF_SIZE]
	__attribute__((used, section(".rtc_noinit")));
static size_t   rtc_tls_session_len
	__attribute__((used, section(".rtc_noinit")));
static uint32_t rtc_tls_session_magic
	__attribute__((used, section(".rtc_noinit")));

/* =========================================================================
 * URL parser
 * ========================================================================= */

static int parse_server_url(const char *url)
{
	/*
	 * Supported forms:
	 *   https://hostname:port/path
	 *   https://[IPv6address]:port/path  (RFC 2732 bracket notation)
	 */
	const char *p = url;

	if (strncmp(p, "https://", 8) != 0) {
		LOG_ERR("LLSS URL must start with https://");
		return -EINVAL;
	}
	p += 8;

	const char *colon;
	const char *slash;

	if (*p == '[') {
		/* IPv6 literal: [addr]:port/path */
		const char *bracket_end = strchr(p, ']');

		if (!bracket_end) {
			LOG_ERR("LLSS URL: unclosed '[' in IPv6 address");
			return -EINVAL;
		}
		size_t hlen = bracket_end - p - 1; /* length without brackets */

		if (hlen >= sizeof(server_host)) {
			return -EINVAL;
		}
		memcpy(server_host, p + 1, hlen);
		server_host[hlen] = '\0';

		colon = bracket_end + 1;
		if (*colon != ':') {
			LOG_ERR("LLSS URL: missing port after IPv6 address");
			return -EINVAL;
		}
		slash = strchr(colon, '/');
	} else {
		/* Regular hostname or IPv4 */
		colon = strchr(p, ':');
		slash = strchr(p, '/');

		if (!colon || (slash && colon > slash)) {
			LOG_ERR("LLSS URL missing port");
			return -EINVAL;
		}

		size_t hlen = colon - p;

		if (hlen >= sizeof(server_host)) {
			return -EINVAL;
		}
		memcpy(server_host, p, hlen);
		server_host[hlen] = '\0';
	}

	/* port */
	server_port = (int32_t)strtol(colon + 1, NULL, 10);

	/* base path */
	if (slash) {
		strncpy(server_base_path, slash, sizeof(server_base_path) - 1);
	} else {
		server_base_path[0] = '\0';
	}

	return 0;
}

/* =========================================================================
 * TLS socket helpers
 * ========================================================================= */

/* =========================================================================
 * DNS resolution without heap — uses dns_get_addr_info() directly
 * ========================================================================= */

/* Maximum addresses to collect: one AAAA + one A */
#define LLSS_DNS_MAX_ADDRS 2

struct llss_dns_state {
	struct k_sem           done;
	struct sockaddr_storage addrs[LLSS_DNS_MAX_ADDRS];
	socklen_t              addrlen[LLSS_DNS_MAX_ADDRS];
	int32_t                count;
};

static void dns_result_cb(enum dns_resolve_status status,
			  struct dns_addrinfo *info,
			  void *user_data)
{
	struct llss_dns_state *state = user_data;

	if (status == DNS_EAI_INPROGRESS && info &&
	    state->count < LLSS_DNS_MAX_ADDRS) {
		memcpy(&state->addrs[state->count], &info->ai_addr,
		       info->ai_addrlen);
		state->addrlen[state->count] = info->ai_addrlen;
		state->count++;
	}

	if (status != DNS_EAI_INPROGRESS) {
		/* DNS_EAI_ALLDONE, DNS_EAI_CANCELED, DNS_EAI_FAIL, … */
		k_sem_give(&state->done);
	}
}

static void invalidate_cached_server_addr(void); /* defined below */

/* Snapshot the currently-configured DNS servers (from the resolver context) as
 * NUL-terminated strings into caller storage. Returns the count; servers[] is
 * NULL-terminated. Used both to recover an inactive context and to reset a
 * wedged one (dns_resolve_close clears ctx->servers, so we must copy first). */
static int collect_dns_servers(
	struct dns_resolve_context *ctx,
	char buf[CONFIG_DNS_RESOLVER_MAX_SERVERS][NET_IPV6_ADDR_LEN],
	const char *servers[CONFIG_DNS_RESOLVER_MAX_SERVERS + 1])
{
	int32_t count = 0;

	for (int32_t i = 0; i < ARRAY_SIZE(ctx->servers) &&
		     count < CONFIG_DNS_RESOLVER_MAX_SERVERS; i++) {
		char *dst = buf[count];
		struct sockaddr *sa = &ctx->servers[i].dns_server;

		if (sa->sa_family == AF_INET) {
			if (!net_addr_ntop(AF_INET,
					   &net_sin(sa)->sin_addr,
					   dst, NET_IPV6_ADDR_LEN)) {
				continue;
			}
		} else if (sa->sa_family == AF_INET6) {
			if (!net_addr_ntop(AF_INET6,
					   &net_sin6(sa)->sin6_addr,
					   dst, NET_IPV6_ADDR_LEN)) {
				continue;
			}
		} else {
			continue;
		}

		servers[count++] = dst;
	}

	servers[count] = NULL;
	return count;
}

static int ensure_dns_context_active(void)
{
	struct dns_resolve_context *ctx = dns_resolve_get_default();

	if (!ctx) {
		return -ENOENT;
	}

	if (ctx->state == DNS_RESOLVE_CONTEXT_ACTIVE) {
		return 0;
	}

	char server_buf[CONFIG_DNS_RESOLVER_MAX_SERVERS][NET_IPV6_ADDR_LEN];
	const char *servers[CONFIG_DNS_RESOLVER_MAX_SERVERS + 1];
	int32_t count = collect_dns_servers(ctx, server_buf, servers);

	if (count == 0) {
		LOG_WRN("DNS context inactive and no server addresses available yet");
		return -ENOENT;
	}

	int32_t ret = dns_resolve_init(ctx, servers, NULL);

	if (ret < 0) {
		LOG_ERR("dns_resolve_init(recover) failed: %d", ret);
		return ret;
	}

	LOG_DBG("DNS context recovered (%d server%s)",
		count, count == 1 ? "" : "s");
	return 0;
}

/* Soft network reset for the server-instability self-heal path: drop the cached
 * server address and close the persistent session so the next attempt starts
 * clean.
 *
 * NOTE (2026-06-18): this deliberately NO LONGER closes/reinitialises the DNS
 * resolver. The earlier "DNS wedge" was a symptom of the reconnect *storm*
 * (buffer exhaustion), which is fixed at the source; the resolver recovers on
 * its own. Repeatedly closing/reiniting a mature subsystem mid-operation just
 * pokes it (and was a suspect for an orphaned RX packet). A genuinely stuck
 * resolver is now left to the watchdog reboot rather than proactive poking. */
void llss_client_net_reset(void)
{
	invalidate_cached_server_addr();
	llss_session_close();
}

static void invalidate_cached_server_addr(void)
{
	cached_server_addr_valid = false;
	cached_server_addrlen = 0;
}

static void cache_server_addr(const struct sockaddr *addr, socklen_t addrlen)
{
	if (addrlen > sizeof(cached_server_addr)) {
		return;
	}

	memcpy(&cached_server_addr, addr, addrlen);
	cached_server_addrlen = addrlen;
	cached_server_addr_valid = true;

	char addr_buf[NET_IPV6_ADDR_LEN];
	int32_t family = addr->sa_family;
	void *raw_addr = NULL;

	if (family == AF_INET) {
		raw_addr = &net_sin(addr)->sin_addr;
	} else if (family == AF_INET6) {
		raw_addr = &net_sin6(addr)->sin6_addr;
	}

	if (raw_addr && net_addr_ntop(family, raw_addr, addr_buf, sizeof(addr_buf))) {
		LOG_DBG("LLSS cache store: %s", addr_buf);
	}
}

static void set_server_port(struct sockaddr *addr)
{
	if (addr->sa_family == AF_INET) {
		net_sin(addr)->sin_port = htons(server_port);
	} else if (addr->sa_family == AF_INET6) {
		net_sin6(addr)->sin6_port = htons(server_port);
	}
}

static void log_tls_socket_state(int32_t sock, const char *phase)
{
	int32_t verify_result = 0;
	int32_t ciphersuite = 0;
	socklen_t optlen;

	optlen = sizeof(verify_result);
	if (zsock_getsockopt(sock, SOL_TLS, TLS_CERT_VERIFY_RESULT,
				    &verify_result, &optlen) == 0) {
		LOG_DBG("LLSS %s: verify_result=0x%x", phase, verify_result);
	}

	optlen = sizeof(ciphersuite);
	if (zsock_getsockopt(sock, SOL_TLS, TLS_CIPHERSUITE_USED,
				    &ciphersuite, &optlen) == 0) {
		LOG_DBG("LLSS %s: ciphersuite=0x%x", phase, ciphersuite);
	}
}

static int connect_tls_addr(const struct sockaddr *addr, socklen_t addrlen)
{
	struct sockaddr_storage target = {0};
	sec_tag_t tls_tags[] = { CONFIG_LLSS_TLS_TAG };
	char addr_buf[NET_IPV6_ADDR_LEN] = "?";
	int32_t cert_nocopy = TLS_CERT_NOCOPY_OPTIONAL;
	int32_t family;
	int32_t sock;
	int32_t rc;
	int32_t saved_errno;
	int64_t start_ms;
	void *raw_addr;

	if (addrlen > sizeof(target)) {
		return -EINVAL;
	}

	memcpy(&target, addr, addrlen);
	set_server_port((struct sockaddr *)&target);
	family = ((struct sockaddr *)&target)->sa_family;
	raw_addr = (family == AF_INET)
		? (void *)&net_sin((struct sockaddr *)&target)->sin_addr
		: (void *)&net_sin6((struct sockaddr *)&target)->sin6_addr;
	if (raw_addr) {
		net_addr_ntop(family, raw_addr, addr_buf, sizeof(addr_buf));
	}

	sock = zsock_socket(family, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (sock < 0) {
		LOG_DBG("socket(af=%d): %d", family, errno);
		return -errno;
	}

	int32_t rcvbuf = CONFIG_LLSS_HTTP_SOCKET_RCVBUF_SIZE;

	rc = zsock_setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
				     &rcvbuf, sizeof(rcvbuf));
	if (rc < 0) {
		LOG_WRN("SO_RCVBUF=%d failed: %d", rcvbuf, errno);
	}

	zsock_setsockopt(sock, SOL_TLS, TLS_SEC_TAG_LIST,
			 tls_tags, sizeof(tls_tags));
	zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
			 server_host, strlen(server_host));
	zsock_setsockopt(sock, SOL_TLS, TLS_CERT_NOCOPY,
			 &cert_nocopy, sizeof(cert_nocopy));

	/* Phase B: enable Zephyr's in-memory session cache so that if this
	 * socket is closed and re-opened within the same boot the TLS
	 * abbreviated handshake (~150 ms) is used instead of a full one. */
	int32_t session_cache_opt = TLS_SESSION_CACHE_ENABLED;

	zsock_setsockopt(sock, SOL_TLS, TLS_SESSION_CACHE,
			 &session_cache_opt, sizeof(session_cache_opt));

	if (llss_ca_cert_len == 0) {
		int32_t peer_verify = TLS_PEER_VERIFY_NONE;

		zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY,
				 &peer_verify, sizeof(peer_verify));
		LOG_WRN("TLS: CA cert not loaded — peer verification disabled");
	}

	start_ms = k_uptime_get();
	LOG_DBG("LLSS connect start: %s (%s)", addr_buf,
		family == AF_INET6 ? "IPv6" : "IPv4");
	rc = zsock_connect(sock, (struct sockaddr *)&target, addrlen);
	if (rc == 0) {
		cache_server_addr((struct sockaddr *)&target, addrlen);
		log_tls_socket_state(sock, "connect ok");

		LOG_DBG("LLSS connect ok: %s in %" PRIi64 " ms",
			addr_buf, k_uptime_get() - start_ms);
		return sock;
	}

	saved_errno = errno;
	log_tls_socket_state(sock, "connect fail");
	LOG_DBG("LLSS connect fail: %s errno=%d after %" PRIi64 " ms",
		addr_buf, saved_errno, k_uptime_get() - start_ms);
	zsock_close(sock);
	return -saved_errno;
}

static int open_tls_socket(void)
{
	struct llss_dns_state dns = { .count = 0 };
	uint16_t dns_id_aaaa, dns_id_a;
	int32_t queries = 0;
	int32_t ret;
	int32_t sock;
	int64_t dns_start_ms;

	if (cached_server_addr_valid) {
		LOG_DBG("LLSS cache hit: trying cached server address");
		sock = connect_tls_addr((struct sockaddr *)&cached_server_addr,
				       cached_server_addrlen);
		if (sock >= 0) {
			LOG_DBG("LLSS cache hit: reused cached address");
			return sock;
		}

		LOG_WRN("Cached server address failed (%d) — re-resolving", sock);
		invalidate_cached_server_addr();
	} else {
		LOG_DBG("LLSS cache miss: resolving %s", server_host);
	}

	ret = ensure_dns_context_active();
	if (ret < 0) {
		LOG_WRN("DNS resolver is not ready yet: %d", ret);
	}

	/* Semaphore limit = 2: one give per completed query */
	k_sem_init(&dns.done, 0, 2);
	dns_start_ms = k_uptime_get();
	LOG_DBG("LLSS DNS start: %s", server_host);
	/* Fire AAAA and A concurrently — DNS_NUM_CONCUR_QUERIES >= 2 */
	if (dns_get_addr_info(server_host, DNS_QUERY_TYPE_AAAA,
			      &dns_id_aaaa, dns_result_cb, &dns,
			      CONFIG_LLSS_DNS_TIMEOUT_MS) == 0) {
		queries++;
	}
	if (dns_get_addr_info(server_host, DNS_QUERY_TYPE_A,
			      &dns_id_a, dns_result_cb, &dns,
			      CONFIG_LLSS_DNS_TIMEOUT_MS) == 0) {
		queries++;
	}

	/* Wait for every query that was successfully submitted */
	for (int32_t i = 0; i < queries; i++) {
		k_sem_take(&dns.done, K_FOREVER);
	}

	if (dns.count == 0) {
		LOG_ERR("DNS resolve %s: no results", server_host);
		return -EHOSTUNREACH;
	}

	LOG_DBG("LLSS DNS done: %d result(s) in %" PRIi64 " ms",
		dns.count, k_uptime_get() - dns_start_ms);

	sock = -1;

	for (int32_t i = 0; i < dns.count && sock < 0; i++) {
		struct sockaddr *addr = (struct sockaddr *)&dns.addrs[i];
		sock = connect_tls_addr(addr, dns.addrlen[i]);
	}

	if (sock < 0) {
		LOG_ERR("connect(): all addresses failed");
		invalidate_cached_server_addr();
		return -EHOSTUNREACH;
	}

	return sock;
}

/* =========================================================================
 * HTTP response accumulator
 * ========================================================================= */

struct resp_ctx {
	uint8_t *buf;
	size_t   buf_size;
	size_t   len;
	int32_t  http_status;
	bool     overflow;
	bool     saw_body;
	uint32_t body_frag_count;
	size_t   total_frag_bytes;
	int64_t  first_body_ms;
	int64_t  last_body_ms;
	int64_t  max_body_gap_ms;
};

static int http_response_cb(struct http_response *rsp,
			     enum http_final_call final,
			     void *user_data)
{
	struct resp_ctx *ctx = user_data;

	if (rsp->http_status_code && !ctx->http_status) {
		ctx->http_status = rsp->http_status_code;
	}

	/* Length of the contiguous body fragment in recv_buf for THIS call.
	 *
	 * Don't trust rsp->body_frag_len directly: when the whole response
	 * arrives in a single recv and http_client's parser hands the body to
	 * its on_body callback in more than one fragment (chunked TE, or a body
	 * split at a record/buffer edge), http_client fires this callback only
	 * once (HTTP_DATA_FINAL) and leaves body_frag_len set to the LAST
	 * fragment's length while body_frag_start still points at the FIRST
	 * fragment — so trusting body_frag_len under-copies the body. The
	 * documented invariant (see struct http_response) is
	 *     body_frag_len == data_len - (body_frag_start - recv_buf)
	 * which always describes the full contiguous body region in recv_buf, so
	 * recompute it that way. Our server returns Content-Length-delimited JSON
	 * (contiguous body), so this recovers the complete body in every case. */
	size_t frag_len = 0;
	if (rsp->body_frag_start != NULL && rsp->recv_buf != NULL) {
		size_t body_off = (size_t)(rsp->body_frag_start - rsp->recv_buf);

		if (rsp->data_len > body_off) {
			frag_len = rsp->data_len - body_off;
		}
	}

	if (frag_len > 0 && ctx->buf && !ctx->overflow) {
		int64_t now_ms = k_uptime_get();

		if (!ctx->saw_body) {
			ctx->saw_body = true;
			ctx->first_body_ms = now_ms;
		} else {
			int64_t gap_ms = now_ms - ctx->last_body_ms;

			if (gap_ms > ctx->max_body_gap_ms) {
				ctx->max_body_gap_ms = gap_ms;
			}
		}

		ctx->last_body_ms = now_ms;

		ctx->body_frag_count++;
		ctx->total_frag_bytes += frag_len;

		/* Leave 1 byte for NUL terminator in JSON buffers */
		size_t room = (ctx->buf_size > ctx->len + 1)
				      ? ctx->buf_size - ctx->len - 1
				      : 0;
		size_t copy = MIN(frag_len, room);

		memcpy(ctx->buf + ctx->len, rsp->body_frag_start, copy);
		ctx->len += copy;
		if (copy < frag_len) {
			ctx->overflow = true;
			LOG_WRN("HTTP response truncated");
		}
	}

	if (final == HTTP_DATA_FINAL && ctx->buf) {
		ctx->buf[ctx->len] = '\0';
	}
	return 0;
}

/* Close a TLS socket after draining any pending inbound data.
 *
 * The server sends a TLS close_notify alert after an HTTP error response (and at
 * connection end). If we zsock_close() with that record still sitting unread in
 * the socket's RX queue, the stack leaks the net_pkt holding it — confirmed by
 * hexdump: one leaked eth_esp32_rx RX segment carrying a `15 03 03` TLS alert
 * from the server per HTTP 502. Reading it lets the TLS layer process the alert
 * and free the segment, which is also the correct way to close a TLS connection
 * (consume the peer's close_notify rather than slamming the socket shut).
 *
 * Bounded (~80 ms worst case) so a chatty or hung peer can't stall us. */
static void drain_and_close(int sock)
{
	uint8_t scratch[128];

	for (int i = 0; i < 8; i++) {
		ssize_t r = zsock_recv(sock, scratch, sizeof(scratch),
				       ZSOCK_MSG_DONTWAIT);

		if (r == 0) {
			break; /* orderly close_notify / EOF consumed — done */
		}
		if (r < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				break; /* real error — nothing more to drain */
			}
			k_msleep(10); /* nothing queued yet — let it arrive */
		}
		/* r > 0: drained some bytes; loop to read the rest incl. the alert */
	}

	zsock_close(sock);
}

/* =========================================================================
 * Generic request helper
 * ========================================================================= */

/**
 * @brief Execute one HTTP request on a fresh TLS connection.
 *
 * @param method        HTTP method (HTTP_GET, HTTP_POST, …)
 * @param path          URL path (with query string if needed)
 * @param auth_header   Full "Authorization: Bearer <token>\r\n" line, or NULL
 * @param body          Request body (JSON string), or NULL
 * @param content_type  Content-Type header value, or NULL
 * @param recv_buf      Buffer for response body
 * @param recv_buf_size Size of recv_buf
 * @param body_len_out  Set to actual body bytes received
 * @return HTTP status code (200, 201, …) on success, negative errno on error.
 */
static int do_request(enum http_method method, const char *path,
		      const char *auth_header,
		      const char *body, const char *content_type,
		      uint8_t *recv_buf, size_t recv_buf_size,
		      size_t *body_len_out)
{
	int64_t req_start_ms = k_uptime_get();
	/* Phase A: reuse persistent session socket if one is open. */
	bool owned_sock = (session_sock < 0);
	int32_t sock = owned_sock ? open_tls_socket() : session_sock;

	if (sock < 0) {
		return sock;
	}

	struct resp_ctx ctx = {
		.buf      = recv_buf,
		.buf_size = recv_buf_size,
	};

	/* Build header array (max 4 extra headers + NULL terminator) */
	const char *headers[8];
	int32_t     hdridx = 0;

	/* auth_header must include "\r\n" at the end */
	static char auth_hdr_buf[LLSS_TOKEN_MAX + 32];

	if (auth_header) {
		snprintf(auth_hdr_buf, sizeof(auth_hdr_buf), "%s", auth_header);
		headers[hdridx++] = auth_hdr_buf;
	}

	static char ctype_hdr_buf[64];

	if (content_type) {
		snprintf(ctype_hdr_buf, sizeof(ctype_hdr_buf),
			 "Content-Type: %s\r\n", content_type);
		headers[hdridx++] = ctype_hdr_buf;
	}

	headers[hdridx] = NULL;

	/* Build a NUL-terminated path on the stack */
	char full_path[LLSS_MAX_URL_LEN];
	int32_t plen = snprintf(full_path, sizeof(full_path), "%s%s",
			     server_base_path, path);

	if (plen < 0 || (size_t)plen >= sizeof(full_path)) {
		if (owned_sock) {
			zsock_close(sock);
		}
		return -EINVAL;
	}

	struct http_request req = {
		.method        = method,
		.url           = full_path,
		.host          = server_host,
		.protocol      = "HTTP/1.1",
		.header_fields = (hdridx > 0) ? headers : NULL,
		.payload       = body,
		.payload_len   = body ? strlen(body) : 0,
		.response      = http_response_cb,
		/* HTTP parse scratch — distinct from ctx.buf (the caller's body
		 * accumulator). See http_recv_scratch declaration. */
		.recv_buf      = http_recv_scratch,
		.recv_buf_len  = sizeof(http_recv_scratch),
	};

	int32_t rc = http_client_req(sock, &req, CONFIG_LLSS_HTTP_TIMEOUT_MS, &ctx);
	int64_t req_end_ms = k_uptime_get();
	bool is_frame_fetch = (method == HTTP_GET) && (strstr(path, "/frames/") != NULL);

	if (is_frame_fetch) {
		int64_t first_body_ms = ctx.saw_body ? (ctx.first_body_ms - req_start_ms) : -1;
		int64_t body_tail_ms = ctx.saw_body ? (req_end_ms - ctx.first_body_ms) : -1;

		LOG_INF("DIAG http frames: path=%s reuse=%d total=%lldms first_body=%lldms tail=%lldms max_gap=%lldms frag_count=%u frag_bytes=%zu status=%d rc=%d body=%zu",
			path, !owned_sock,
			(long long)(req_end_ms - req_start_ms),
			(long long)first_body_ms,
			(long long)body_tail_ms,
			(long long)ctx.max_body_gap_ms,
			ctx.body_frag_count,
			ctx.total_frag_bytes,
			ctx.http_status,
			rc,
			ctx.len);
	}

	if (owned_sock) {
		drain_and_close(sock);
	} else if (rc < 0) {
		/* Shared session socket has a real TRANSPORT failure (TLS broken,
		 * connection reset, timeout) — invalidate it so the next call
		 * opens a fresh connection.
		 *
		 * We deliberately DO NOT close on an HTTP error status (>=400,
		 * e.g. a reverse-proxy 502).  Per HTTP keep-alive semantics the
		 * connection lifetime is governed by Connection:close, NOT the
		 * response code: a 502 means the proxy's *upstream* is down, while
		 * our TLS connection to the proxy is still perfectly healthy.
		 * Closing it on every 502 churned the connection on each poll and
		 * stranded un-ACKed tcp_out segments → slow TX-pool drain → crash.
		 * Keep the persistent session through HTTP errors; the caller backs
		 * off and retries over it.  If the peer genuinely did send a
		 * close_notify with the error, the NEXT request on this socket
		 * returns rc<0 and is torn down cleanly right here. */
		LOG_WRN("LLSS session socket error (rc=%d) — invalidating", rc);
		drain_and_close(session_sock);
		session_sock = -1;
	}

	if (rc < 0) {
		LOG_ERR("http_client_req %s: %d", full_path, rc);
		return rc;
	}

	if (body_len_out) {
		*body_len_out = ctx.len;
	}

	return ctx.http_status;
}

/* =========================================================================
 * Phase A — Public session management API
 * ========================================================================= */

int llss_session_open(void)
{
	if (session_sock >= 0) {
		return 0; /* already open */
	}

	session_sock = open_tls_socket();
	if (session_sock < 0) {
		int32_t err = session_sock;

		session_sock = -1;
		return err;
	}

	LOG_DBG("LLSS session open: sock=%d", session_sock);
	return 0;
}

void llss_session_close(void)
{
	if (session_sock < 0) {
		return;
	}

	LOG_DBG("LLSS session close: sock=%d", session_sock);
	drain_and_close(session_sock);
	session_sock = -1;
}

/* =========================================================================
 * Build Authorization header strings
 * ========================================================================= */

static void make_bearer_header(const char *token, char *buf, size_t len)
{
	snprintf(buf, len, "Authorization: Bearer %s\r\n", token);
}

/* =========================================================================
 * JSON descriptors
 * ========================================================================= */

/* --- DeviceRegistrationResponse --- */
struct reg_resp {
	char device_id[65];
	char device_secret[65];
	char auth_status[16];
	char message[128];
};

static const struct json_obj_descr reg_resp_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct reg_resp, device_id,     JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct reg_resp, device_secret, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct reg_resp, auth_status,   JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct reg_resp, message,       JSON_TOK_STRING_BUF),
};

/* --- DeviceAuthResponse (token endpoint) --- */
struct auth_resp {
	char device_id[65];
	char refresh_token[LLSS_TOKEN_MAX];
	char auth_status[16];
	char message[128];
};

static const struct json_obj_descr auth_resp_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct auth_resp, device_id,     JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct auth_resp, refresh_token, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct auth_resp, auth_status,   JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct auth_resp, message,       JSON_TOK_STRING_BUF),
};

/* --- TokenRefreshResponse --- */
struct token_resp {
	char access_token[LLSS_TOKEN_MAX];
	char token_type[16];
};

static const struct json_obj_descr token_resp_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct token_resp, access_token, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct token_resp, token_type,   JSON_TOK_STRING_BUF),
};

/* --- RefreshRenewalResponse --- */
struct renewal_resp {
	char refresh_token[LLSS_TOKEN_MAX];
};

static const struct json_obj_descr renewal_resp_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct renewal_resp, refresh_token, JSON_TOK_STRING_BUF),
};

/* --- DeviceStateResponse --- */
struct state_resp {
	char action[16];
	char frame_id[64];
	char active_instance_id[64];
	int32_t poll_after_ms;
};

static const struct json_obj_descr state_resp_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct state_resp, action,             JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct state_resp, frame_id,           JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct state_resp, active_instance_id, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct state_resp, poll_after_ms,      JSON_TOK_NUMBER),
};

/* --- InputProcessResponse --- */
struct input_resp {
	char status[16];
	char frame_id[64];
	int32_t poll_after_ms;
	char message[128];
};

static const struct json_obj_descr input_resp_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct input_resp, status,        JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct input_resp, frame_id,      JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct input_resp, poll_after_ms, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct input_resp, message,       JSON_TOK_STRING_BUF),
};

/* =========================================================================
 * auth_status string → enum
 * ========================================================================= */

static enum llss_auth_status parse_auth_status(const char *s)
{
	if (strcmp(s, "authorized") == 0) {
		return LLSS_AUTH_AUTHORIZED;
	}
	if (strcmp(s, "pending") == 0) {
		return LLSS_AUTH_PENDING;
	}
	if (strcmp(s, "rejected") == 0) {
		return LLSS_AUTH_REJECTED;
	}
	if (strcmp(s, "revoked") == 0) {
		return LLSS_AUTH_REVOKED;
	}
	return LLSS_AUTH_UNKNOWN;
}

/* =========================================================================
 * Public API implementation
 * ========================================================================= */

int llss_client_init(void)
{
	if (client_initialised) {
		return 0;
	}

	int32_t rc = parse_server_url(CONFIG_LLSS_SERVER_URL);

	if (rc) {
		return rc;
	}

	/* Register CA certificate (zero-length = stub, skip) */
	if (llss_ca_cert_len > 0) {
		rc = tls_credential_add(CONFIG_LLSS_TLS_TAG,
					TLS_CREDENTIAL_CA_CERTIFICATE,
					llss_ca_cert, llss_ca_cert_len);
		if (rc && rc != -EEXIST) {
			LOG_ERR("tls_credential_add: %d", rc);
			return rc;
		}
	} else {
		LOG_WRN("CA cert not loaded — TLS peer verification will be "
			"disabled for LLSS connections");
	}

	client_initialised = true;
	LOG_INF("LLSS client ready: host=%s port=%d base=%s",
		server_host, server_port, server_base_path);
	return 0;
}

/* ----- Register ---------------------------------------------------------- */

int llss_register(const char *hardware_id,
		  char *device_id_out, char *device_secret_out)
{
	char body[256];

	snprintf(body, sizeof(body),
		 "{\"hardware_id\":\"%s\","
		 "\"firmware_version\":\"%s\","
		 "\"display\":{\"width\":800,\"height\":480,"
		 "\"bit_depth\":%d,\"partial_refresh\":false,"
		 "\"top_strip_height\":%d,\"bottom_strip_height\":%d}}",
		 hardware_id,
		 CONFIG_LLSS_FIRMWARE_VERSION,
		 CONFIG_LLSS_DISPLAY_BIT_DEPTH,
		 CONFIG_LLSS_TOP_STRIP_HEIGHT,
		 CONFIG_LLSS_BOTTOM_STRIP_HEIGHT);

	int32_t http_status = do_request(HTTP_POST, "/auth/devices/register",
				     NULL, body, "application/json",
				     json_recv_buf, sizeof(json_recv_buf),
				     NULL);

	if (http_status < 0) {
		return http_status;
	}

	/* 201 = new registration, 409 = already registered (re-read stored) */
	if (http_status != 201 && http_status != 200) {
		LOG_ERR("register HTTP %d: %s", http_status, json_recv_buf);
		return http_status;
	}

	struct reg_resp resp = {0};
	int32_t rc = json_obj_parse((char *)json_recv_buf,
				strnlen((char *)json_recv_buf,
					sizeof(json_recv_buf)),
				reg_resp_descr, ARRAY_SIZE(reg_resp_descr),
				&resp);

	if (rc < 0) {
		LOG_ERR("register JSON parse: %d", rc);
		return rc;
	}

	strncpy(device_id_out,     resp.device_id,     LLSS_DEVICE_ID_MAX - 1);
	strncpy(device_secret_out, resp.device_secret, LLSS_DEVICE_SECRET_MAX - 1);

	LOG_INF("Registered: device_id=%s auth_status=%s",
		resp.device_id, resp.auth_status);
	return 0;
}

/* ----- Authenticate ------------------------------------------------------ */

int llss_authenticate(const char *hardware_id, const char *device_secret,
		      enum llss_auth_status *auth_status_out,
		      char *refresh_token_out)
{
	char body[256];

	snprintf(body, sizeof(body),
		 "{\"hardware_id\":\"%s\","
		 "\"device_secret\":\"%s\","
		 "\"firmware_version\":\"%s\","
		 "\"display\":{\"width\":800,\"height\":480,"
		 "\"bit_depth\":%d,\"partial_refresh\":false,"
		 "\"top_strip_height\":%d,\"bottom_strip_height\":%d}}",
		 hardware_id, device_secret,
		 CONFIG_LLSS_FIRMWARE_VERSION,
		 CONFIG_LLSS_DISPLAY_BIT_DEPTH,
		 CONFIG_LLSS_TOP_STRIP_HEIGHT,
		 CONFIG_LLSS_BOTTOM_STRIP_HEIGHT);

	int32_t http_status = do_request(HTTP_POST, "/auth/devices/token",
				     NULL, body, "application/json",
				     json_recv_buf, sizeof(json_recv_buf),
				     NULL);

	if (http_status < 0) {
		return http_status;
	}

	if (http_status == 401 || http_status == 403) {
		*auth_status_out    = LLSS_AUTH_REJECTED;
		refresh_token_out[0] = '\0';
		return -EACCES;
	}

	if (http_status != 200) {
		LOG_ERR("authenticate HTTP %d: %s", http_status, json_recv_buf);
		return http_status;
	}

	struct auth_resp resp = {0};
	int32_t rc = json_obj_parse((char *)json_recv_buf,
				strnlen((char *)json_recv_buf,
					sizeof(json_recv_buf)),
				auth_resp_descr, ARRAY_SIZE(auth_resp_descr),
				&resp);

	if (rc < 0) {
		LOG_ERR("authenticate JSON parse: %d", rc);
		return rc;
	}

	*auth_status_out = parse_auth_status(resp.auth_status);
	strncpy(refresh_token_out, resp.refresh_token, LLSS_TOKEN_MAX - 1);

	LOG_INF("Authenticate: status=%s", resp.auth_status);
	return 0;
}

/* ----- Refresh access token ---------------------------------------------- */

int llss_refresh_access_token(const char *refresh_token,
			      char *access_token_out)
{
	char auth_hdr[LLSS_TOKEN_MAX + 32];

	make_bearer_header(refresh_token, auth_hdr, sizeof(auth_hdr));

	int32_t http_status = do_request(HTTP_POST, "/auth/devices/refresh",
				     auth_hdr, NULL, NULL,
				     json_recv_buf, sizeof(json_recv_buf),
				     NULL);

	if (http_status < 0) {
		return http_status;
	}

	if (http_status == 400 || http_status == 401 || http_status == 403) {
		return -EACCES;
	}

	if (http_status != 200) {
		LOG_ERR("refresh HTTP %d", http_status);
		return http_status;
	}

	struct token_resp resp = {0};
	int32_t rc = json_obj_parse((char *)json_recv_buf,
				strnlen((char *)json_recv_buf,
					sizeof(json_recv_buf)),
				token_resp_descr, ARRAY_SIZE(token_resp_descr),
				&resp);

	if (rc < 0) {
		LOG_ERR("refresh JSON parse: %d", rc);
		return rc;
	}

	strncpy(access_token_out, resp.access_token, LLSS_TOKEN_MAX - 1);
	LOG_DBG("New access token obtained");
	return 0;
}

/* ----- Renew refresh token ----------------------------------------------- */

int llss_renew_refresh_token(const char *access_token,
			     char *refresh_token_out)
{
	char auth_hdr[LLSS_TOKEN_MAX + 32];

	make_bearer_header(access_token, auth_hdr, sizeof(auth_hdr));

	int32_t http_status = do_request(HTTP_POST, "/auth/devices/renew-refresh",
				     auth_hdr, NULL, NULL,
				     json_recv_buf, sizeof(json_recv_buf),
				     NULL);

	if (http_status < 0) {
		return http_status;
	}

	if (http_status == 401 || http_status == 403) {
		return -EACCES;
	}

	if (http_status != 200) {
		LOG_ERR("renew-refresh HTTP %d", http_status);
		return http_status;
	}

	struct renewal_resp resp = {0};
	int32_t rc = json_obj_parse((char *)json_recv_buf,
				strnlen((char *)json_recv_buf,
					sizeof(json_recv_buf)),
				renewal_resp_descr,
				ARRAY_SIZE(renewal_resp_descr),
				&resp);

	if (rc < 0) {
		LOG_ERR("renew-refresh JSON parse: %d", rc);
		return rc;
	}

	strncpy(refresh_token_out, resp.refresh_token, LLSS_TOKEN_MAX - 1);
	LOG_INF("Refresh token renewed");
	return 0;
}

/* ----- Poll device state ------------------------------------------------- */

int llss_get_device_state(const char *access_token, const char *device_id,
			  const char *last_frame_id,
			  struct llss_device_state *state_out)
{
	char path[LLSS_MAX_URL_LEN];
	char auth_hdr[LLSS_TOKEN_MAX + 32];

	if (last_frame_id && last_frame_id[0]) {
		snprintf(path, sizeof(path),
			 "/devices/%s/state?last_frame_id=%s",
			 device_id, last_frame_id);
	} else {
		snprintf(path, sizeof(path), "/devices/%s/state", device_id);
	}

	make_bearer_header(access_token, auth_hdr, sizeof(auth_hdr));

	int32_t http_status = do_request(HTTP_GET, path, auth_hdr, NULL, NULL,
				     json_recv_buf, sizeof(json_recv_buf),
				     NULL);

	if (http_status < 0) {
		return http_status;
	}

	if (http_status == 401 || http_status == 403) {
		return -EACCES;
	}

	if (http_status != 200) {
		LOG_ERR("device state HTTP %d", http_status);
		return http_status;
	}

	struct state_resp resp = {
		.poll_after_ms = CONFIG_LLSS_POLL_INTERVAL_MS,
	};

	int32_t rc = json_obj_parse((char *)json_recv_buf,
				strnlen((char *)json_recv_buf,
					sizeof(json_recv_buf)),
				state_resp_descr,
				ARRAY_SIZE(state_resp_descr),
				&resp);

	if (rc < 0) {
		LOG_ERR("device state JSON parse: %d", rc);
		return rc;
	}

	/* Map action string → enum */
	if (strcmp(resp.action, "FETCH_FRAME") == 0) {
		state_out->action = LLSS_ACTION_FETCH_FRAME;
	} else if (strcmp(resp.action, "SLEEP") == 0) {
		state_out->action = LLSS_ACTION_SLEEP;
	} else if (strcmp(resp.action, "NOOP") == 0) {
		/* Explicit "nothing to do" — poll again, no warning. */
		state_out->action = LLSS_ACTION_NOOP;
	} else {
		LOG_WRN("Unknown state action: %s", resp.action);
		state_out->action = LLSS_ACTION_NOOP;
	}

	strncpy(state_out->frame_id, resp.frame_id,
		sizeof(state_out->frame_id) - 1);

	state_out->poll_after_ms = CLAMP(resp.poll_after_ms,
					 CONFIG_LLSS_MIN_POLL_MS,
					 CONFIG_LLSS_MAX_POLL_MS);

	return 0;
}

/* ----- Fetch frame ------------------------------------------------------- */

int llss_fetch_frame(const char *access_token, const char *device_id,
		     const char *frame_id,
		     uint8_t *dst, size_t dst_size, size_t *len_out)
{
	char path[LLSS_MAX_URL_LEN];
	char auth_hdr[LLSS_TOKEN_MAX + 32];

	/* ?raw=true => panel-native packed framebuffer (1bpp: 48000 B, MSB-first)
	 * instead of a PNG. Blitted straight to panel RAM — no on-device decode. */
#if defined(CONFIG_LLSS_PATTERN_TEST)
	/* &pattern=1 => server returns the deterministic self-test pattern. */
	snprintf(path, sizeof(path), "/devices/%s/frames/%s?raw=true&pattern=1",
		 device_id, frame_id);
#else
	snprintf(path, sizeof(path), "/devices/%s/frames/%s?raw=true",
		 device_id, frame_id);
#endif
	make_bearer_header(access_token, auth_hdr, sizeof(auth_hdr));

	/* HTTP body lands directly in the caller's buffer (a display pipeline
	 * slot) — no intermediate copy. */
	size_t body_len = 0;
	int32_t http_status = do_request(HTTP_GET, path, auth_hdr, NULL, NULL,
				     dst, dst_size, &body_len);

	if (http_status < 0) {
		return http_status;
	}

	if (http_status == 401 || http_status == 403) {
		return -EACCES;
	}

	if (http_status != 200) {
		LOG_ERR("fetch frame HTTP %d", http_status);
		return http_status;
	}

	if (body_len == 0) {
		LOG_ERR("fetch frame: empty body");
		return -ENODATA;
	}

	*len_out = body_len;

	return 0;
}

/* ----- Fetch pressed strip (protocol extension) -------------------------- */

int llss_fetch_pressed_strip(const char *access_token, const char *device_id,
			     const char *frame_id, enum llss_strip strip,
			     uint8_t *dst, size_t dst_size, size_t *len_out)
{
	char path[LLSS_MAX_URL_LEN];
	char auth_hdr[LLSS_TOKEN_MAX + 32];
	const char *which = (strip == LLSS_STRIP_TOP_PRESSED)
				    ? "top_pressed" : "bottom_pressed";

	snprintf(path, sizeof(path), "/devices/%s/frames/%s?strip=%s",
		 device_id, frame_id, which);
	make_bearer_header(access_token, auth_hdr, sizeof(auth_hdr));

	size_t body_len = 0;
	int32_t http_status = do_request(HTTP_GET, path, auth_hdr, NULL, NULL,
				     dst, dst_size, &body_len);

	if (http_status < 0) {
		return http_status;
	}

	if (http_status == 401 || http_status == 403) {
		return -EACCES;
	}

	/* No pressed strip for this frame/server — expected, not an error.
	 * Caller falls back to local feedback (or none). */
	if (http_status == 404) {
		return -ENOENT;
	}

	if (http_status != 200) {
		LOG_ERR("fetch pressed strip HTTP %d", http_status);
		return http_status;
	}

	if (body_len == 0) {
		return -ENODATA;
	}

	*len_out = body_len;
	return 0;
}

/* ----- Send input -------------------------------------------------------- */

int llss_send_input(const char *access_token, const char *device_id,
		    const char *button_name, const char *event_type,
		    struct llss_input_response *resp_out)
{
	char path[LLSS_MAX_URL_LEN];
	char auth_hdr[LLSS_TOKEN_MAX + 32];
	char body[256];

	snprintf(path, sizeof(path), "/devices/%s/inputs", device_id);
	make_bearer_header(access_token, auth_hdr, sizeof(auth_hdr));

	/* ISO 8601 timestamp from CLOCK_REALTIME.  ntp_sync owns this clock
	 * and only sets SYS_FLAG_TIME_VALID once tv_sec >= 2024-01-01, so by
	 * the time we reach STATE_POLLING the clock is believable.  Fall back
	 * to epoch if the read fails (server accepts both). */
	char ts[24] = "1970-01-01T00:00:00Z";
	struct timespec now = {0};
	struct tm tm_buf;

	if (sys_clock_gettime(SYS_CLOCK_REALTIME, &now) == 0 &&
	    gmtime_r(&now.tv_sec, &tm_buf) != NULL) {
		strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
	}

	snprintf(body, sizeof(body),
		 "{\"button\":\"%s\","
		 "\"event_type\":\"%s\","
		 "\"timestamp\":\"%s\"}",
		 button_name, event_type, ts);

	int32_t http_status = do_request(HTTP_POST, path, auth_hdr,
				     body, "application/json",
				     json_recv_buf, sizeof(json_recv_buf),
				     NULL);

	if (http_status < 0) {
		return http_status;
	}

	if (http_status == 401 || http_status == 403) {
		return -EACCES;
	}

	if (http_status != 200) {
		LOG_ERR("send_input HTTP %d: %s", http_status, json_recv_buf);
		return http_status;
	}

	struct input_resp resp = {
		.poll_after_ms = CONFIG_LLSS_POLL_INTERVAL_MS,
	};

	int32_t rc = json_obj_parse((char *)json_recv_buf,
				strnlen((char *)json_recv_buf,
					sizeof(json_recv_buf)),
				input_resp_descr, ARRAY_SIZE(input_resp_descr),
				&resp);

	if (rc < 0) {
		LOG_ERR("send_input JSON parse: %d", rc);
		return rc;
	}

	/* Map status string → enum */
	if (strcmp(resp.status, "NEW_FRAME") == 0) {
		resp_out->status = LLSS_INPUT_NEW_FRAME;
	} else if (strcmp(resp.status, "NO_CHANGE") == 0) {
		resp_out->status = LLSS_INPUT_NO_CHANGE;
	} else if (strcmp(resp.status, "POLL") == 0) {
		resp_out->status = LLSS_INPUT_POLL;
	} else {
		resp_out->status = LLSS_INPUT_ERROR;
	}

	strncpy(resp_out->frame_id, resp.frame_id,
		sizeof(resp_out->frame_id) - 1);
	resp_out->poll_after_ms = CLAMP(resp.poll_after_ms,
					CONFIG_LLSS_MIN_POLL_MS,
					CONFIG_LLSS_MAX_POLL_MS);
	return 0;
}
