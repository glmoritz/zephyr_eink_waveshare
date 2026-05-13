# Migration: drop Phase B.2 patch → CONFIG_NET_SOCKETS_TLS_SESSION_CACHE_PERSISTENT

## Context

The Zephyr toolchain at `/opt/toolchains/zephyr` has been upgraded to a version
that contains `CONFIG_NET_SOCKETS_TLS_SESSION_CACHE_PERSISTENT` in
`subsys/net/lib/sockets/sockets_tls.c` (first appeared after Zephyr 4.3.0, visible
in main at commit a7464a0).

This upstream feature replaces the hand-rolled Phase B.2 in `llss_client.c` that
stored TLS session bytes in RTC slow RAM using `TLS_SESSION_EXPORT` /
`TLS_SESSION_IMPORT` socket options from the local patch.

**Verify the feature is present before starting:**
```sh
grep -n "TLS_SESSION_CACHE_PERSISTENT" \
    /opt/toolchains/zephyr/subsys/net/lib/sockets/sockets_tls.c \
    /opt/toolchains/zephyr/subsys/net/lib/sockets/Kconfig
```
Both files must return matches. If they do not, stop — the toolchain has not been
upgraded yet.

Also confirm the patch is no longer applied (it must NOT be present):
```sh
grep -n "TLS_SESSION_EXPORT\|TLS_SESSION_IMPORT" \
    /opt/toolchains/zephyr/include/zephyr/net/socket.h
```
If matches exist the patch is still applied; remove it first.

---

## Files to change

### 1. `/workspace/projects/hello_eink/prj.conf`

**Remove** the Phase B.2 comment block (lines referencing `.rtc_noinit`):
```
# Phase B: the TLS session buffer in llss_client.c is placed in .rtc_noinit
# via __attribute__((section(".rtc_noinit"))).  No Kconfig reservation is
# needed — the ESP32-S3 RTC slow RAM (8 KB) is always available.
```

**Add** after the existing `CONFIG_NET_SOCKETS_TLS_MAX_CLIENT_SESSION_COUNT=1` line:
```kconfig
# Phase B.2 (upstream): persist the TLS session cache to NVS across reboots
# and deep sleep.  Replaces the hand-rolled RTC RAM + TLS_SESSION_EXPORT path.
# Requires CONFIG_SETTINGS=y + CONFIG_SETTINGS_NVS=y (already set below).
CONFIG_NET_SOCKETS_TLS_SESSION_CACHE_PERSISTENT=y
CONFIG_NET_SOCKETS_TLS_SESSION_CACHE_PERSISTENT_PREFIX="tls/sess"
```

`CONFIG_SETTINGS=y` and `CONFIG_SETTINGS_NVS=y` are already present — no change
needed there.

---

### 2. `/workspace/projects/hello_eink/src/llss_client.c`

**Keep all RTC RAM code.** Do not delete `rtc_tls_session_buf`,
`rtc_tls_session_len`, `rtc_tls_session_magic`, the `TLS_SESSION_IMPORT`
block, or the `TLS_SESSION_EXPORT` block.

The two persistence mechanisms are complementary:

| Wake type | Mechanism | Source |
|-----------|-----------|--------|
| Deep sleep (DRAM lost, RTC RAM intact) | `TLS_SESSION_IMPORT` from `rtc_tls_session_buf` | RTC RAM — no flash read |
| Cold boot / power loss | `CONFIG_NET_SOCKETS_TLS_SESSION_CACHE_PERSISTENT` | NVS via Settings |

`CONFIG_NET_SOCKETS_TLS_SESSION_CACHE_PERSISTENT` fills the gap when RTC RAM is
lost (power cycle); the RTC RAM path is still faster on every deep-sleep wake
because it avoids the flash read entirely.

#### 2a. Add local fallback defines if the patch is removed

`TLS_SESSION_EXPORT` (21) and `TLS_SESSION_IMPORT` (22) were defined in the
patch's additions to `include/zephyr/net/socket.h`.  Once the patch is removed
they must be defined locally so the file compiles.  Add these lines near the
top of `llss_client.c` with the other `#define` constants:

```c
/* Fallback defines for TLS session export/import socket options.
 * These were added by the local Zephyr patch; if that patch is removed and
 * upstream Zephyr does not yet define them, the setsockopt/getsockopt calls
 * below will return -ENOPROTOOPT and fall back gracefully. */
#ifndef TLS_SESSION_EXPORT
#define TLS_SESSION_EXPORT 21
#endif
#ifndef TLS_SESSION_IMPORT
#define TLS_SESSION_IMPORT 22
#endif
```

On a non-patched Zephyr the socket options return `ENOPROTOOPT`; the existing
error-handling branches already treat any non-zero return as a graceful
fallback (full handshake).

#### 2b. Keep the Phase B in-memory cache line unchanged

This line must remain — it is still used for within-boot reconnect acceleration:
```c
int session_cache_opt = TLS_SESSION_CACHE_ENABLED;
zsock_setsockopt(sock, SOL_TLS, TLS_SESSION_CACHE,
		 &session_cache_opt, sizeof(session_cache_opt));
```

---

### 3. Delete the patch file

The patch is no longer needed:
```sh
rm /workspace/patches/zephyr_tls_session_export.patch
```

Confirm it is not referenced anywhere:
```sh
grep -r "zephyr_tls_session_export" /workspace --include="*.cmake" \
    --include="CMakeLists.txt" --include="*.sh" --include="*.yml"
```
If any references remain, remove them too.

---

### 4. Update `protocol_improvements.md`

In `/workspace/projects/hello_eink/protocol_improvements.md`, change the Phase B.2
row in the summary table from:

```
| B.2: RTC RAM persist | deferred (needs PM + Zephyr API) | 1 (resumed) | **~150 ms** | none |
```

to:

```
| B.2: NVS persist (upstream) | ✅ done | 1 (resumed) | **~150 ms** | none |
```

Also update the Phase B.2 section body to note that the feature landed upstream
and is now enabled via `CONFIG_NET_SOCKETS_TLS_SESSION_CACHE_PERSISTENT=y` in
`prj.conf`, and that the RTC RAM + patch approach has been removed.

---

## Verification after the change

1. Build succeeds with no reference to `TLS_SESSION_EXPORT`, `TLS_SESSION_IMPORT`,
   `rtc_tls_session_buf`, or `LLSS_TLS_SESSION_MAGIC` in compile output.
2. On first boot after flash: serial log shows full handshake (~3 s).
3. On second boot (NVS populated): serial log shows abbreviated handshake
   (~150 ms) — look for `"connect ok ... in <500 ms"`.
4. After `settings_delete("tls/sess/...")` or NVS erase: falls back to full
   handshake transparently.

## What stays the same

- Phase A (connection reuse via `session_sock`) — unchanged.
- Phase B in-memory cache (`TLS_SESSION_CACHE_ENABLED`) — unchanged.
- All other `llss_client.c` logic, `main.c`, `llss_storage.c` — unchanged.
- `CONFIG_MBEDTLS_SSL_SESSION_TICKETS=y` — still required for TLS 1.3 tickets.
