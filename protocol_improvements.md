# LLSS Protocol Improvements — Sleep/Wake Latency Roadmap

**Context:** ESP32-S3 with Zephyr 4.3.0 + mbedTLS (software-only, no HW accelerator).  
**Problem:** Every `do_request()` call previously opened a fresh TLS socket, causing
a full ECDHE+certificate handshake (~3 s on this hardware) per API call.

---

## Baseline (before this work)

| Wake cycle            | Handshakes | Crypto time |
|-----------------------|-----------|-------------|
| First boot (register → token → refresh → poll → fetch) | 4 | ~12 s |
| Steady-state (poll + fetch)                             | 2 | ~6 s  |
| With token refresh                                      | 4 | ~12 s |

At a 1–5 min wake interval this consumed 4–10 % of the battery budget in crypto.

---

## Phase A — Connection Reuse  ✅ implemented

**Files changed:** `llss_client.c`, `llss_client.h`, `main.c`  
**Server changes:** none

`llss_api_thread_fn` in `main.c` now calls `llss_session_open()` before dispatching
any job and `llss_session_close()` after.  `do_request()` checks the module-level
`session_sock`; if it is `>= 0` it reuses the open socket instead of calling
`open_tls_socket()`.

- **Result:** 2–4 handshakes per wake → **1 handshake per wake cycle**
- First boot: ~12 s → **~3 s**
- Steady-state: ~6 s → **~3 s**

Socket error handling: if `http_client_req()` fails on a shared socket (broken
pipe, server-side close), `do_request()` closes and invalidates `session_sock` so
the next call within the same job reopens a fresh one transparently.

---

## Phase B — TLS Session Cache (within-boot reconnect acceleration) ✅ implemented

**Files changed:** `llss_client.c`, `prj.conf`  
**Server changes:** none

`connect_tls_addr()` now sets the `TLS_SESSION_CACHE_ENABLED` socket option on every
new socket.  Zephyr's built-in in-memory session cache (1 slot,
`CONFIG_NET_SOCKETS_TLS_MAX_CLIENT_SESSION_COUNT=1`) stores the negotiated session.

If `session_sock` is closed unexpectedly and the job retries the connection to the
same peer address, the abbreviated TLS handshake (~150 ms) is used instead of the
full one (~3 s).

**Limitation:** Zephyr 4.3's `CONFIG_MBEDTLS_SSL_SESSION_TICKETS` is gated on TLS 1.3
(which we do not use).  Cross-sleep session persistence (Phase B.2 below) is therefore
deferred.

### Phase B.2 — Cross-sleep RTC RAM persistence (deferred)

The RTC slow RAM buffer (`rtc_tls_session_buf[512]` in `.rtc_noinit`) is already
declared in `llss_client.c` and survives deep sleep.  Full use is blocked by two
prerequisites:

1. **`CONFIG_PM`** — deep sleep is not yet implemented in this firmware.
2. **Zephyr TLS socket API gap** — `IPPROTO_TLS_1_2` sockets hide the
   `mbedtls_ssl_context`, so `mbedtls_ssl_session_save()` / `mbedtls_ssl_session_load()`
   cannot be called from application code without bypassing the Zephyr wrapper.

When both are resolved, the implementation is:

```c
/* after successful handshake in connect_tls_addr() */
mbedtls_ssl_session sess;
if (mbedtls_ssl_get_session(&ssl, &sess) == 0) {
    size_t len = 0;
    mbedtls_ssl_session_save(&sess, rtc_tls_session_buf,
                             sizeof(rtc_tls_session_buf), &len);
    rtc_tls_session_len   = len;
    rtc_tls_session_magic = LLSS_TLS_SESSION_MAGIC;
    mbedtls_ssl_session_free(&sess);
}

/* before handshake on wake */
if (rtc_tls_session_magic == LLSS_TLS_SESSION_MAGIC &&
    rtc_tls_session_len > 0) {
    mbedtls_ssl_session sess;
    if (mbedtls_ssl_session_load(&sess, rtc_tls_session_buf,
                                 rtc_tls_session_len) == 0) {
        mbedtls_ssl_set_session(&ssl, &sess);
        mbedtls_ssl_session_free(&sess);
    }
}
```

Expected result: first wake after sleep uses resumed handshake (~150 ms) instead of
fresh (~3 s).  Falls back to full handshake transparently if ticket is stale (e.g.
server restarted — daily at worst given NGINX's default 24 h ticket lifetime).

---

## Phase C — PSK-TLS for steady-state (planned, ~1 week)

**Goal:** ~50 ms handshake on every cold wake, regardless of ticket state.  
`TLS_PSK_WITH_AES_128_GCM_SHA256` uses only symmetric crypto — no ECDHE, no
certificate chain parsing.

### Server side

1. Add `psk` field (32-byte hex) to `DeviceAuthResponse` and
   `TokenRefreshResponse` in the API; generate with `secrets.token_bytes(32)`.
2. Store per-device PSK in DB (`device_psk BYTEA`).
3. Deploy **stunnel 5** as PSK-TLS proxy on port 9607 (alongside Traefik on 9608):

   ```ini
   [llss-psk]
   accept  = 9607
   connect = 127.0.0.1:8080   ; same LLSS backend
   PSKsecrets = /etc/llss/psk.txt
   ciphers = PSK-AES128-GCM-SHA256
   ```

   `psk.txt` format: `device_id:hex_psk` (one per line).  LLSS rewrites this
   file atomically and sends `SIGHUP` to stunnel on each PSK issue/rotation.
   stunnel reads PSK identity from the TLS `ClientHello` to look up the key —
   no other authentication needed.

4. Traefik is untouched.  stunnel is a sidecar.

### Client side

- `prj.conf`: add `CONFIG_MBEDTLS_KEY_EXCHANGE_PSK_ENABLED=y`
- After cert-TLS auth: store `psk` + `device_id` (as PSK identity) in NVS.
- `connect_tls_addr()`: if PSK present in NVS, configure
  `mbedtls_ssl_conf_psk()` and connect to port 9607.  On `ECONNABORTED`/401
  fall back to cert-TLS on port 9608, re-authenticate, fetch new PSK.
- PSK rotation: on every `POST /auth/devices/renew-refresh` (every ~15 days).

**Trade-off:** No forward secrecy (`TLS_PSK_WITH_AES_128_GCM_SHA256` has no
ECDHE).  Acceptable: data is e-ink screen content; PSK rotation on token renewal
limits the exposure window to ~15 days.

---

## Phase D — WireGuard (planned, after Zephyr 4.4 matures)

**Goal:** ~20 ms re-keying on every wake, full forward secrecy, no per-connection
TLS, app-layer JWT auth unchanged.

### Architecture

```
ESP32 → WireGuard UDP → pfSense wg0 → LLSS host (plain HTTP)
```

- pfSense WireGuard: add device subnet (e.g. `10.6.0.0/24`) as a new peer group.
  Each device gets a `/32` AllowedIPs entry.  This scales to thousands of peers
  (WireGuard state per peer is ~60 bytes in the kernel; pfSense handles it natively).
- LLSS provisions WG peers on registration:
  `POST /auth/devices/register` body gains `wg_pubkey` field; LLSS calls
  `wg set wg0 peer <pubkey> allowed-ips 10.6.0.x/32` (or pfSense API).
- Subsequent connections: WireGuard Noise IKpsk2 handshake → 2 UDP round trips
  → ~20 ms on WiFi, then plain HTTP over the tunnel.
- Session re-keying every 3 min is transparent (kernel handles it on first packet
  after wake).

**Why deferred:** Zephyr 4.4 WireGuard support is brand new.  Revisit when it
has seen production use.  Phase C (PSK-TLS) is the practical near-term solution.

---

## Summary table

| Phase | Status | Handshakes/wake | Time/wake | Server change |
|-------|--------|----------------|-----------|---------------|
| Baseline | — | 2–4 | 6–12 s | — |
| A: connection reuse | ✅ done | **1** | **~3 s** | none |
| B: session cache | ✅ done | 1 (fallback < 1) | **~3 s** (fallback ~150 ms) | none |
| B.2: RTC RAM persist | deferred (needs PM + Zephyr API) | 1 (resumed) | **~150 ms** | none |
| C: PSK-TLS | planned | 1 PSK | **~50 ms** | stunnel + DB column |
| D: WireGuard | deferred (Zephyr 4.4) | 1 WG rekey | **~20 ms** | pfSense peer + wg set |

### Verification

| Phase | Serial log indicator |
|-------|---------------------|
| A | "LLSS connect ok" appears exactly once per wake cycle |
| B | After unexpected reconnect: "connect ok ... in <500 ms" |
| B.2 | After sleep/wake: "connect ok ... in <500 ms" (vs ~3000 ms) |
| C | Wireshark on port 9607: no `Certificate` records in handshake |
| D | Wireshark: WireGuard UDP frames, no TLS records |
