#!/usr/bin/env bash
#
# Networking setup for native_sim builds.
#
# Runs INSIDE the dev container (which is started with --privileged and
# CAP_NET_ADMIN). Creates a TAP interface "zeth" with the container side
# at 192.0.2.2 (IPv4) and fd00:dead::1/64 (IPv6), routes those subnets
# out via NAT, and runs radvd so the sim picks up its IPv6 via SLAAC.
#
#   Sim guest side: 192.0.2.1   /  fd00:dead::<EUI-64>
#   Container side: 192.0.2.2   /  fd00:dead::1
#
# eink.tutu.eng.br is IPv6-only, so the v6 path is the one that carries
# real LLSS traffic; the v4 path is kept for DNS / fallback / tooling.
#
# Idempotent — re-running will not duplicate state. Tear down with
# `pkill radvd; ip link delete zeth`.
#
# Requirements (installed in Dockerfile.moritz): iproute2, iptables, radvd.
# Also requires the Docker daemon to have ipv6 + ip6tables enabled in
# /etc/docker/daemon.json so the container itself has v6 connectivity.
# The container already runs as root, so no sudo is involved.

set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
	echo "error: must run as root. The dev container's default user is root —" >&2
	echo "       run this from inside the container, not from the host." >&2
	exit 1
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

IFACE=zeth
V4_GATEWAY=192.0.2.2
V4_SUBNET=192.0.2.0/24
V6_GATEWAY=fd00:dead::1
V6_SUBNET=fd00:dead::/64
RADVD_CONF="${SCRIPT_DIR}/radvd-zeth.conf"
RADVD_PID=/run/radvd-zeth.pid

UPLINK="$(ip route show default | awk '/^default/ {print $5; exit}')"
if [[ -z "${UPLINK}" ]]; then
	echo "error: could not detect default uplink interface" >&2
	exit 1
fi

UPLINK_V6="$(ip -6 route show default | awk '/^default/ {print $5; exit}')"
if [[ -z "${UPLINK_V6}" ]]; then
	echo "warning: container has no IPv6 default route." >&2
	echo "         Enable ipv6 + ip6tables in /etc/docker/daemon.json on the host," >&2
	echo "         restart docker, then re-open the dev container." >&2
fi

# ---- interface --------------------------------------------------------------
if ! ip link show "${IFACE}" >/dev/null 2>&1; then
	ip tuntap add "${IFACE}" mode tap
fi
ip link set "${IFACE}" up
ip addr replace "${V4_GATEWAY}/24"   dev "${IFACE}"
ip -6 addr replace "${V6_GATEWAY}/64" dev "${IFACE}"

# ---- forwarding -------------------------------------------------------------
sysctl -w net.ipv4.ip_forward=1               >/dev/null
sysctl -w net.ipv6.conf.all.forwarding=1      >/dev/null
sysctl -w "net.ipv6.conf.${IFACE}.forwarding=1" >/dev/null

# ---- IPv4 NAT ---------------------------------------------------------------
if ! iptables -t nat -C POSTROUTING -s "${V4_SUBNET}" -o "${UPLINK}" -j MASQUERADE 2>/dev/null; then
	iptables -t nat -A POSTROUTING -s "${V4_SUBNET}" -o "${UPLINK}" -j MASQUERADE
fi
if ! iptables -C FORWARD -i "${IFACE}" -o "${UPLINK}" -j ACCEPT 2>/dev/null; then
	iptables -A FORWARD -i "${IFACE}" -o "${UPLINK}" -j ACCEPT
fi
if ! iptables -C FORWARD -i "${UPLINK}" -o "${IFACE}" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null; then
	iptables -A FORWARD -i "${UPLINK}" -o "${IFACE}" -m state --state RELATED,ESTABLISHED -j ACCEPT
fi

# ---- IPv6 NAT66 -------------------------------------------------------------
if [[ -n "${UPLINK_V6}" ]]; then
	if ! ip6tables -t nat -C POSTROUTING -s "${V6_SUBNET}" -o "${UPLINK_V6}" -j MASQUERADE 2>/dev/null; then
		ip6tables -t nat -A POSTROUTING -s "${V6_SUBNET}" -o "${UPLINK_V6}" -j MASQUERADE
	fi
	if ! ip6tables -C FORWARD -i "${IFACE}" -o "${UPLINK_V6}" -j ACCEPT 2>/dev/null; then
		ip6tables -A FORWARD -i "${IFACE}" -o "${UPLINK_V6}" -j ACCEPT
	fi
	if ! ip6tables -C FORWARD -i "${UPLINK_V6}" -o "${IFACE}" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null; then
		ip6tables -A FORWARD -i "${UPLINK_V6}" -o "${IFACE}" -m state --state RELATED,ESTABLISHED -j ACCEPT
	fi
fi

# ---- radvd (SLAAC) ----------------------------------------------------------
if [[ -f "${RADVD_PID}" ]] && kill -0 "$(cat "${RADVD_PID}")" 2>/dev/null; then
	kill "$(cat "${RADVD_PID}")"
	sleep 0.2
fi
if command -v radvd >/dev/null 2>&1; then
	radvd -C "${RADVD_CONF}" -p "${RADVD_PID}"
	echo "radvd advertising ${V6_SUBNET} on ${IFACE}"
else
	echo "warning: radvd not installed — rebuild the container image to pick it up." >&2
fi

echo "zeth ready:"
echo "  IPv4 gateway=${V4_GATEWAY}  sim=192.0.2.1     NAT via ${UPLINK}"
echo "  IPv6 gateway=${V6_GATEWAY}  sim=fd00:dead::*  NAT via ${UPLINK_V6:-<none>}"
