#!/usr/bin/env bash
#
# Launch the native_sim build of hello_eink.
#
# Assumes:
#   - The project has been built: `west build -b native_sim/native/64`
#   - The zeth TAP is up: `./scripts/setup_net_sim.sh`
#
# The flash backing file persists NVS / settings / TLS session cache across
# runs, so cross-boot resume can be exercised by stopping and restarting the
# binary without rebuilding.

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build-sim}"
FLASH_FILE="${FLASH_FILE:-${BUILD_DIR}/hello_eink_flash.bin}"
ZEPHYR_EXE="${BUILD_DIR}/zephyr/zephyr.exe"

if [[ ! -x "${ZEPHYR_EXE}" ]]; then
	echo "error: ${ZEPHYR_EXE} not found. Build with:" >&2
	echo "  west build -b native_sim/native/64 -d ${BUILD_DIR}" >&2
	exit 1
fi

if ! ip link show zeth >/dev/null 2>&1; then
	echo "warning: zeth interface missing — run scripts/setup_net_sim.sh first" >&2
fi

# native_sim accepts single-dash flags (per its --help output).
exec "${ZEPHYR_EXE}" \
	-eth-if=zeth \
	-flash="${FLASH_FILE}" \
	"$@"
