# Building and running `hello_eink`

The same source tree builds for two targets:

| Target | Board | Purpose |
|---|---|---|
| Hardware | `eink_llss_esp32/esp32s3/procpu` | Final firmware for the ESP32-S3 + Waveshare 800×480 4-shade e-ink panel. |
| Simulator | `native_sim/native/64` | Linux x86-64 binary with an SDL window standing in for the panel and a TAP interface standing in for WiFi. Used to validate the LLSS API flow, TLS, token exchange, and rendering pipeline without flashing. |

Both targets share `prj.conf`, all of `src/`, the LVGL pipeline, mbedTLS / TLS stack, NVS settings, and the LLSS client. Only the bottom-of-stack pieces (display driver, WiFi provisioning, RTC chip, power-management hardware) swap between targets.

All commands below assume you are inside the dev container shell, with the workspace at `/workspace` and the active project at `/workspace/projects/hello_eink`.

---

## ESP32-S3 (hardware)

### Build

The shared workspace ships a helper that sets the active project and runs `west build` with the right board + patches:

```sh
bash /workspace/scripts/zephyr-project.sh set /workspace/projects/hello_eink
bash /workspace/scripts/zephyr-project.sh build
```

Equivalent direct invocation if you want to skip the helper:

```sh
cd /workspace/projects/hello_eink
west build -p auto -b eink_llss_esp32/esp32s3/procpu -- -DEXTRA_CONF_FILE=debug.conf
```

The board name is set by the per-project `.board` file and the debug Kconfig overlay is auto-applied when `debug.conf` is present.

### Flash

USB-JTAG flashing via the helper:

```sh
bash /workspace/scripts/zephyr-project.sh flash            # auto-detects the port
bash /workspace/scripts/zephyr-project.sh flash /dev/ttyACM0
```

Or direct:

```sh
west flash
```

### Serial console

The application logs to USB CDC-ACM (the same `/dev/ttyACM0` Zephyr uses for the shell):

```sh
python3 /workspace/monitor.py /dev/ttyACM0 115200
```

### OpenOCD debug

```sh
bash /workspace/scripts/zephyr-project.sh openocd
```

This launches OpenOCD against the ESP32-S3 with the bundled config; attach with GDB from VS Code or directly:

```sh
$ZEPHYR_SDK_INSTALL_DIR/xtensa-espressif_esp32s3_zephyr-elf/bin/xtensa-espressif_esp32s3_zephyr-elf-gdb \
  /workspace/projects/hello_eink/build/zephyr/zephyr.elf \
  -ex 'target remote :3333'
```

---

## native_sim (simulator)

The simulator build mirrors everything that doesn't require ESP32-S3 hardware. The e-ink panel is replaced by an SDL window at the real 800×480 resolution; WiFi provisioning is replaced by a stub module that announces "connected" half a second after boot (the TAP interface is already up by then). Storage uses Zephyr's file-backed flash simulator, so NVS / settings / TLS session cache persist across `zephyr.exe` runs in exactly the same way they persist across cold boots on hardware.

### One-time host display setup

`zephyr.exe` opens an SDL window inside the dev container; the container forwards it to the host X server (or Xwayland, on a Wayland session). The wiring is in `.devcontainer/devcontainer.json` — it bind-mounts `/tmp/.X11-unix` and inherits `DISPLAY` from your host shell. Two host-side steps make it work:

1. **Allow the container to talk to your X server.** Run this once per host login session (it does not survive a logout):

   ```sh
   xhost +local:
   ```

   This grants any local user — including the container's root — access to the X socket. To revoke later: `xhost -local:`.

2. **Rebuild the dev container** so it picks up the new mount and env var. In VS Code: *Dev Containers: Rebuild Container*. From the CLI: stop the running container and let your usual launch flow recreate it.

This setup works identically under an X11 session (Ubuntu 24.04 default with "Ubuntu on Xorg") and under a Wayland session (Ubuntu 24.04 default GNOME, Ubuntu 26.04). On Wayland, Xwayland transparently provides the X socket — SDL inside the container connects to it without knowing the host is on Wayland.

Sanity check from inside the container after rebuild:

```sh
echo "$DISPLAY"          # should match your host (e.g. :0 or :1)
ls /tmp/.X11-unix/       # should list X0, X1, etc.
xeyes &                  # optional: needs x11-apps in the container; quick visual confirmation
```

### One-time host-side Docker IPv6 setup

`eink.tutu.eng.br` is IPv6-only, so the simulator needs working v6 connectivity all the way out. Docker's default bridge is v4-only, so this is a one-time daemon-level change.

Add these three keys to `/etc/docker/daemon.json` (anywhere at the top level):

```json
"ipv6": true,
"ip6tables": true,
"fixed-cidr-v6": "fd00:d0c5::/64"
```

Then:

```sh
sudo systemctl restart docker
```

This restarts the Docker daemon, which kills every running container. Containers with restart policies come back automatically; the dev container itself needs to be re-opened from VS Code (*Dev Containers: Reopen in Container*).

With `ip6tables: true`, Docker automatically masquerades the bridge prefix (`fd00:d0c5::/64`) out your host's default v6 route, so the container has working v6 internet as soon as it starts.

Sanity check from inside the container:

```sh
ip -6 addr show eth0           # should show an address from fd00:d0c5::/64
timeout 5 bash -c '</dev/tcp/eink.tutu.eng.br/9608' && echo OK
```

### One-time networking setup (runs inside the container)

The sim talks to the outside world via a TAP interface called `zeth`, created **inside the dev container** — not on the host. The container is started with `--privileged` (see `.devcontainer/devcontainer.json`), which gives it `CAP_NET_ADMIN` so `ip`, `iptables`, `ip6tables`, and `radvd` work; the container's default user is `root`, so no `sudo` is involved.

The script:

- assigns `192.0.2.2/24` and `fd00:dead::1/64` to `zeth`,
- enables IPv4 + IPv6 forwarding,
- sets up MASQUERADE in both `iptables` (v4) and `ip6tables` (NAT66) toward the container's uplink,
- starts `radvd` so the sim auto-configures its IPv6 address via SLAAC — same code path the ESP32 build exercises when its WiFi router emits RAs.

```sh
cd /workspace/projects/hello_eink
./scripts/setup_net_sim.sh
```

Expected output:

```
radvd advertising fd00:dead::/64 on zeth
zeth ready:
  IPv4 gateway=192.0.2.2     sim=192.0.2.1     NAT via eth0
  IPv6 gateway=fd00:dead::1  sim=fd00:dead::*  NAT via eth0
```

Re-running is safe — the script is idempotent. Tear down with `pkill radvd; ip link delete zeth`; it also vanishes when the container restarts.

The script depends on `iproute2`, `iptables`, and `radvd` being installed in the image (added in `Dockerfile.moritz`). If you get *"command not found"* or *"radvd not installed"*, rebuild the container so the new packages land: *Dev Containers: Rebuild Container without Cache*.

If you want to talk to a local LLSS instance only (no internet), skip the script and just bring up `zeth` yourself with whatever routing you need; the sim itself doesn't care.

### Build

```sh
cd /workspace/projects/hello_eink
west build -p auto -b native_sim/native/64 -d build-sim
```

A separate build directory (`build-sim`) keeps the simulator artifacts from clobbering the ESP32 `build/` tree, so you can flip between targets without a clean rebuild.

### Run

```sh
./scripts/run_native_sim.sh
```

The script launches `build-sim/zephyr/zephyr.exe` with:

- `-eth-if=zeth` — bind the simulated Ethernet driver to the TAP
- `-flash=build-sim/hello_eink_flash.bin` — file-backed flash image (created on first run)

What you'll see:

1. An SDL window opens (800×480, 8-bit greyscale).
2. The stub provisioning module logs `Simulated WiFi connected` after 500 ms.
3. The LLSS state machine starts: DNS, TLS handshake against `eink.tutu.eng.br`, registration / token exchange, frame fetch.
4. LVGL renders the received frame into the SDL window.

Any flags passed to `run_native_sim.sh` are forwarded to `zephyr.exe`. Useful ones:

```sh
./scripts/run_native_sim.sh -rt                  # slow execution to real time
./scripts/run_native_sim.sh -flash_erase         # wipe persistent state at boot
./scripts/run_native_sim.sh -rtc-offset=86400    # advance simulated RTC by a day
./scripts/run_native_sim.sh --help               # full list
```

### Resetting persistent state

The flash backing file holds NVS, settings, and the TLS session cache. To exercise cold-boot behaviour from a clean slate:

```sh
rm build-sim/hello_eink_flash.bin
```

…or pass `-flash_erase` on a single run to zero it without deleting the file.

### Debugging

`zephyr.exe` is a regular x86-64 ELF, so gdb works directly:

```sh
gdb build-sim/zephyr/zephyr.exe
(gdb) run -eth-if=zeth -flash=build-sim/hello_eink_flash.bin
```

Or attach to a running instance with `gdb -p <pid>`. Breakpoints, watchpoints, and full source-level stepping all work because everything is host-native — this is the main reason the sim is useful for chasing protocol bugs.

---

## What's the same, what's different

| Subsystem | ESP32-S3 | native_sim |
|---|---|---|
| Application code (`src/`) | identical | identical |
| LVGL + frame pipeline | identical | identical (8 bpp luminance) |
| mbedTLS / TLS / HTTP client | identical | identical |
| LLSS client + state machine | identical | identical |
| NVS + settings | NVS on internal flash | NVS on file-backed flash simulator |
| Display driver | custom SSD16xx (`src/display_ssd16xx_800x480.c`) | `zephyr,sdl-dc` |
| Network | WiFi via the `wifi_prov` module + captive portal | TAP via stub `wifi_prov_sim` module |
| RTC | PCF85063A over I²C | `zephyr,rtc-emul` |
| Power management | AXP2101 PMIC + charger + fuel gauge | not built |
| Deep sleep / `.rtc_noinit` | ESP32 RTC slow RAM (8 KB) | section attribute degrades to plain BSS |

The sim deliberately does **not** model deep-sleep semantics. Cross-sleep TLS resume and other power-cycle-specific behaviours must still be validated on hardware. Everything before that — the LLSS API, token lifecycle, certificate chain, TLS handshake performance, frame decoding and rendering — runs faithfully on the host.
