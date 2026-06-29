# The net-buffer leak adventure

A postmortem of the ESP32-S3 / Zephyr net-buffer exhaustion that crashed the
LLSS e-ink client after ~12–15 h of operation, the three genuine upstream Zephyr
bugs it surfaced, and the application-level root cause that was actually driving
it.

Platform: `eink_llss_esp32/esp32s3/procpu`, Zephyr 4.4.99 (`v4.4.0-2951-gfbfc5f19636`),
TLS-over-WiFi HTTP client polling an IPv6-only server.

---

## TL;DR

- **Symptom:** device ran fine for hours, then `esp32_wifi: Failed to allocate net
  buffer` storms, then a fatal `load-prohibited` exception (`VADDR 0x11`) in
  `llss_thread`. Net pools (RX `net_pkt`/`net_buf`, then TX) slowly drained to zero.
- **Three real Zephyr bugs were found and fixed** — all in *teardown* paths:
  1. NS solicitation `net_pkt` leaked when a neighbor request is already pending.
  2. RX `net_pkt` stranded by a close/recv TOCTOU race in the socket layer.
  3. L2 dereferences a header-less `net_pkt` (NULL) under pool exhaustion → kernel halt.
- **But the actual driver of exhaustion was our own code:** the client tore down
  and re-established the TLS connection on **every HTTP error (502)**. Under an
  unstable server that is constant connection churn, which stranded TCP
  retransmit-queue segments (`tcp_out`) and hammered the rare teardown paths above.
- **The cure** was to stop churning: keep the persistent connection through HTTP
  errors, close only on real transport failure. Verified by an 18.6 h soak under a
  server flapping every 8 minutes — net pools stayed flat at full, zero leaks, no
  crash (pre-fix it crashed at ~15 h).
- The three Zephyr fixes remain as **defense-in-depth** (and are upstream-worthy).

---

## Timeline / how it was hunted

1. **Observation:** overnight the pools were exhausted and the device had panicked.
   Dumping leaked packets (`net allocs`, `net pkt <addr>`) showed mostly RX segments
   carrying a TLS `close_notify` — "the majority were the close, but not all of them."
2. **Methodology:** rather than guess, added surgical lifecycle instrumentation
   (`CONFIG_LLSS_PKT_LIFETIME` → greppable `PKTL` printk at alloc / send / free with
   caller), plus a periodic `NETMEM` pool-telemetry line, and a server-flap soak to
   reproduce on demand.
3. **Sim A/B:** native_sim with the same stack over a TAP interface never leaked —
   the trigger was platform/timing dependent, not generic logic.
4. **Each leak was isolated one variable at a time** (see the three bugs below),
   fixed, and re-soaked. Each fix peeled back a layer and revealed the next.
5. **The meta-realization** (credit: skepticism that *three* bugs in battle-tested
   code was suspicious): the bugs are real, but they live in teardown corners a
   well-behaved embedded client rarely touches. We were exercising teardown ~1000×
   more than any real device would, by churning connections. Fixing the churn made
   the leaks stop firing in practice.

---

## Bug 1 — NS solicitation leak (TX)

`subsys/net/ip/ipv6_nbr.c`, `net_ipv6_send_ns()`.

When a Neighbor Solicitation is built but the target neighbor already has a
solicitation pending, the function queues the caller's data packet and returns
early ("Let the system timeout and then send the NS again") **without freeing the
NS packet it just allocated**. The only success-path unref is in `net_send_data()`
→ L2 send, which is past the early return. Every packet sent to an INCOMPLETE
neighbor that already has a pending solicitation leaks one TX `net_pkt`.

Triggered here when the server/gateway neighbor had to be re-resolved on reconnect
(a burst of ≥2 packets to an INCOMPLETE neighbor leaks N-1).

**Fix:** `net_pkt_unref(pkt)` before the already-pending early return.
Patch: `patches/zephyr_ipv6_nbr_ns_pending_leak.patch`.

## Bug 2 — RX recv-queue close race (RX)

`subsys/net/ip/net_context.c` (`net_context_packet_received`) +
`subsys/net/lib/sockets/sockets_inet.c` (`zsock_received_cb`, `zsock_close`).

`net_context_packet_received()` validates `context->recv_cb` under
`context->lock`, then invokes the receive callback **after releasing the lock**.
Concurrently `zsock_close()` clears `recv_cb` (via `net_context_recv(NULL)`) and
then drains `ctx->recv_q` with `zsock_flush_queue()`. A TCP segment that already
passed the `recv_cb != NULL` check and unlocked gets appended to `ctx->recv_q`
**after the flush already ran**. Nothing re-drains `recv_q` (`tcp_conn_release()`
only drains `conn->recv_data`), so the packet is stranded forever.

Proven on hardware: a probe at the enqueue gated on `recv_cb == NULL` fired 40
times over a soak and **all 40 packets leaked = exactly the 40-deep RX pool**, a
1:1:1 correlation between the racing enqueue and the leak.

**Fix:** guard the enqueue — if `recv_cb == NULL` the socket is closing, so unref
the packet instead of queuing it. Safe because `zsock_close()` clears `recv_cb`
*before* flushing: non-NULL ⇒ flush hasn't run ⇒ enqueue will be drained; NULL ⇒
flush ran/won't re-run ⇒ drop (the app has closed; identical outcome to the flush).
Patch: `patches/zephyr_sockets_recvq_close_race_leak.patch`.

## Bug 3 — L2 NULL deref on a header-less packet (the crash)

`subsys/net/l2/ethernet/ethernet.c`, `ethernet_fill_header()` and its mcast/bcast
destination helpers.

The helpers read `NET_IPV4_HDR(pkt)->dst` / `NET_IPV6_HDR(pkt)->dst` with no check
that the packet has an IP header. `net_pkt_ip_data()` is literally
`pkt->frags->data`, so a header-less packet (`frags == NULL`, which happens when
`net_pkt` allocation fails under TX-pool exhaustion) produces a NULL-region load —
`EXCCAUSE 28` (load prohibited), `VADDR 0x11` (the dst field at IP-header offset
16/17 off a NULL header) — and **halts the kernel**.

This is the fatal exception we chased from day one. The death chain:
TX pool drains to 0 → an outgoing IPv4 multicast/broadcast packet
(mDNS/LLMNR/IGMP-class) can't get a buffer → unchecked NULL → L2 deref → halt.

**Fix:** guard the three helpers with `net_pkt_is_empty(pkt)` (short-circuits on
`!buffer`) so a header-less packet is treated as not-mcast/bcast and falls through
to the resolved L2 destination instead of dereferencing NULL. A packet must never
be able to halt the kernel.
Patch: `patches/zephyr_ethernet_mcast_null_guard.patch`.

> All three bugs were verified present in upstream **Zephyr v4.4.1 (latest release)
> and `main`** by reading the source — not artifacts of our snapshot. They are
> backport/PR candidates (draft PR writeups: `pr_*.md` at repo root).

---

## The real root cause — connection churn (our code)

The three bugs above are real, but they are all *teardown*-path bugs. Battle-tested
networking code is battle-tested on the **common path** (steady keep-alive RX/TX).
We were hammering teardown because of one line in `do_request()` (`src/llss_client.c`):

```c
} else if (ctx.http_status >= 400) {
    drain_and_close(session_sock);   // close the TLS connection on EVERY 502
    session_sock = -1;
}
```

This is **wrong by HTTP semantics**: a persistent connection's lifetime is governed
by `Connection: close` / keep-alive, *independent of the response status*. A 502 is
a reverse proxy reporting that its *upstream* is down — our TLS connection *to the
proxy* is still healthy. Closing it on every 502 meant, under a flapping server, we
destroyed and rebuilt the connection on essentially every poll. That churn:

- stranded the in-flight TCP `tcp_out` retransmit-queue segments (the slow TX drain
  that ultimately caused the crash), and
- exercised the rare close/teardown paths (Bugs 1–3) thousands of times more than a
  real device ever would.

Reference embedded clients do the opposite: ESP-IDF's `esp_http_client` reuses one
connection across requests and closes only when the *server* sends `Connection:
close` — never on an error status. We had been coding like a backend service
(reconnect is cheap, RAM is infinite); on an MCU every reconnect is a TLS handshake
burning `net_pkt`s + CPU and a roll of the dice on the cold paths.

### The fix
`do_request()` now closes the persistent session **only on a real transport failure**
(`rc < 0`: TLS broken, reset, timeout). It keeps the connection through HTTP errors;
the caller backs off and retries over it (`do_poll` already clamps the poll interval
to never-zero and backs off). If the peer genuinely sent `close_notify` with an
error, the *next* request returns `rc < 0` and the connection is torn down cleanly
right there. (The DNS-resolver reinit was already removed in a prior pass.)

### Verification
18.6 h soak with the server flapping up/down every 8 minutes (a deliberately
unstable connection): `pktTX 16/16`, `pktRX 40/40`, TX/RX DATA full, **zero stranded
allocs, no crash**, system heap flat. Pre-fix, the same flap drained TX from the
start and crashed at ~15 h.

---

## Known residual risk (read this)

The no-churn fix removes the *driver* of exhaustion, and the three Zephyr fixes
remove the *leaks* and the *halt*. **But a subtle residual remains** worth stating
plainly:

- Closing a TCP connection to an **unreachable peer** (server fully down, or Wi-Fi
  dropped) cannot complete the FIN handshake, so a few un-ACKed `tcp_out` segments
  can sit in the retransmit queue until TCP times out and aborts the connection.
  This is bounded to ~one connection's worth per close event, *not* the unbounded
  churn we fixed.
- Over a **chronically unstable connection** (frequent Wi-Fi flaps / hard server
  outages), these bounded strands could, in the worst case, accumulate slowly. The
  per-event rate is tiny, so realistic exhaustion would take **weeks to months of
  continuous awake operation** — not the ~12–15 h we used to see.
- **It will never surface in normal operation, because of deep sleep.** The device's
  duty cycle deep-sleeps between updates; deep sleep loses RAM, so the `net_pkt` /
  `net_buf` pools are re-initialized fresh on every wake. Strands cannot accumulate
  across sleep cycles. The old crash only happened because the debugging/soak builds
  stayed *continuously awake* polling for many hours.

In short: with deep sleep in the normal duty cycle, this is a non-issue. The only
way to hit it is to keep the device awake on a flaky link for weeks. If a future
use case requires long continuous-awake operation on an unstable link, the remaining
item to investigate is whether Zephyr's `tcp_conn_release()` frees the TCP
**send/retransmit** queue on abort (it is confirmed to free the *recv* queue).

---

## Production checklist

- [ ] **Arm the watchdog reboot.** `CONFIG_LLSS_WATCHDOG_DRY_RUN` defaults to `y` —
      soft resets are live (close+reopen session at 60 s offline, exp-capped 600 s),
      but the reboot backstop only *logs* "would reset" instead of rebooting. Set
      `=n` for production so a deep wedge self-heals (reboot at 300 s offline, exp
      backoff capped 24 h, persisted in RTC RAM). This is the one config flip needed.
- [ ] Battery note: while offline the device stays awake polling (it never receives
      the server's `SLEEP` action). Fine on mains; add an "offline → deep-sleep N,
      retry" rule if ever battery-powered.
- [ ] Offline UX is minimal (stale frame / status banner) — deferred, cosmetic.

---

## State of this tree (cleanup done)

- **Application fix** is in `src/llss_client.c` (`do_request`, close-on-transport-error-only).
- **Three Zephyr fixes** live in `patches/` and are auto-applied by the shared build
  helper:
  - `zephyr_ipv6_nbr_ns_pending_leak.patch`
  - `zephyr_sockets_recvq_close_race_leak.patch`
  - `zephyr_ethernet_mcast_null_guard.patch`
- **Debug instrumentation removed** from the Zephyr SDK tree (the `PKTL` lifecycle
  printks in `ipv6_nbr.c`, `esp_wifi_drv.c`, `net_pkt.c`, `sockets_inet.c` are gone;
  `CONFIG_LLSS_PKT_LIFETIME` is now an inert toggle). The lightweight `NETMEM`
  pool-telemetry (`CONFIG_LLSS_NETMEM_DEBUG`) and `CONFIG_NET_DEBUG_NET_PKT_ALLOC`
  are still enabled in `debug.conf` for ongoing monitoring of the residual risk
  above; a true production build (no `debug.conf`) drops them.
- Upstream PR drafts: `pr_ipv6_ns_pending_leak.md`, `pr_sockets_recvq_close_race_leak.md`
  at repo root (the L2 guard warrants a third).
