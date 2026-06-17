<!--
SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
SPDX-License-Identifier: Apache-2.0
-->

# LLSS Transport Profiling Notes

This note records the LLSS frame-fetch latency investigation carried out in
June 2026 for the ESP32-S3 hardware target and the `native_sim` target.

## Goal

Reduce the latency of the raw 1 bpp frame fetch path:

- request: `GET /devices/{device_id}/frames/{frame_id}?raw=true`
- payload size: `48000` bytes
- focus: transport and buffering, not PNG decode or display submission

## Main findings

1. The initial large delay was not caused by the server application.
2. The largest early win on `native_sim` came from receive-side TCP tuning.
3. The current remaining latency is mostly on the response-arrival side.
4. Request transmit time is negligible.
5. Persistent session reuse is working again, but it was not the main limiter.

## Instrumentation added

The following temporary diagnostics were added during the investigation.

### App-level LLSS diagnostics

In [src/llss_client.c](src/llss_client.c):

- `DIAG http frames`
- fields:
  - `reuse`
  - `total`
  - `first_body`
  - `tail`
  - `max_gap`
  - `frag_count`
  - `frag_bytes`
  - `status`
  - `rc`
  - `body`

This is the main summary line for each frame fetch.

In [src/llss_thread.c](src/llss_thread.c):

- `DIAG send_input`
- `DIAG fetch`
- `DIAG submit`

These were used to separate button submission latency, HTTP fetch latency, and
display pipeline latency.

### Zephyr HTTP client diagnostics

In [/opt/toolchains/zephyr/subsys/net/lib/http/http_client.c](/opt/toolchains/zephyr/subsys/net/lib/http/http_client.c):

- `DIAG http rx`
- `DIAG http tx`

`DIAG http rx` prints one line per receive burst with:

- `poll_wait`
- `idle`
- `recv`
- `offset`
- `total`

`DIAG http tx` prints the request send time for frame fetches.

Example observed result on `native_sim`:

```text
DIAG http tx: url=/api/devices/.../frames/...?... send=0ms bytes=321
```

This showed that request transmit time is effectively zero and that the
remaining latency sits on the response path.

## Session reuse issue found and fixed

The LLSS thread had a local `session_open` flag in
[src/llss_thread.c](src/llss_thread.c) while the actual socket state lived in
[src/llss_client.c](src/llss_client.c).

That allowed this mismatch:

- `llss_client` invalidated `session_sock`
- `llss_thread` still believed the session was open
- the next request silently fell back to an owned socket
- frame fetch logs showed `reuse=0`

The fix was to make `session_ensure()` always call `llss_session_open()` and let
the client module be the source of truth for socket liveness.

Result:

- session reuse returned to `reuse=1`
- reconnect handshakes no longer happened accidentally between button input and
  frame fetch

## Receive-path tuning performed

The receive path was increased several times to understand the bottlenecks.

Final current settings in the repo are:

In [Kconfig](Kconfig):

- `CONFIG_LLSS_HTTP_RECV_SCRATCH_SIZE=65535`
- `CONFIG_LLSS_HTTP_SOCKET_RCVBUF_SIZE=65535`

In [prj.conf](prj.conf):

- `CONFIG_NET_CONTEXT_RCVBUF=y`
- `CONFIG_NET_BUF_DATA_SIZE=512`
- `CONFIG_NET_PKT_RX_COUNT=40`
- `CONFIG_NET_BUF_RX_COUNT=160`

In [boards/native_sim_64.conf](boards/native_sim_64.conf):

- simulator-specific giant-window tuning was removed
- the simulator now follows the shared receive-path settings while still using
  its fixed MTU setup

## What the measurements showed

### Earlier baseline

An earlier native run showed roughly:

- `total=1380ms`
- `first_body=240ms`
- `tail=1140ms`
- many small fragments

### After receive-side tuning

Later native runs improved to roughly:

- `total=300ms`
- `first_body=180ms`
- `tail=120ms`

### Hardware with reuse restored

An ESP32-S3 run after the reuse fix showed:

```text
DIAG http frames: ... reuse=1 total=379ms first_body=279ms tail=100ms ...
```

That result mattered because it showed:

- the session reuse fix was real
- but session reuse was not the dominant remaining bottleneck

### Key interpretation

The current probes indicate:

- transmit time is essentially zero
- response delivery arrives in bursts
- repeated `4096` reads are not coming from the LLSS app buffer anymore
- the remaining floor is likely TLS record availability and/or response-side
  pacing in the stack and network path

## Logging changes made during investigation

In [debug.conf](debug.conf):

- socket/TLS log level was reduced from debug to info

This was done because `net_sock` and `net_sock_tls` debug output was too noisy
for interpreting the transport probes. The `printk` diagnostics remain active.

## Memory impact

The receive-path tuning significantly increased internal DRAM usage.

One measured ESP32-S3 build reached roughly:

- `dram0_0_seg: 85.69%`

This still builds and runs, but it is no longer a low-cost change. Any future
increase in receive-side memory should be justified with measured latency wins.

## Current conclusion

At the present state of the investigation:

1. Session reuse is fixed.
2. Request send time is not a problem.
3. RX memory is already heavily provisioned.
4. The remaining latency is most likely not solved by simply growing buffers
   further.

If this work resumes later, the next high-value probe is likely one level below
the HTTP client, around `mbedtls_ssl_read()` in Zephyr's TLS socket layer.