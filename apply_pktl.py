#!/usr/bin/env python3
# Apply surgical CONFIG_LLSS_PKT_LIFETIME printk instrumentation to the Zephyr
# SDK tree inside the container. Idempotent-ish: refuses to double-apply.
import sys, shutil, os

ZED = "/opt/toolchains/zephyr"

def patch(path, anchor, insert, tag):
    full = ZED + path
    with open(full) as f:
        src = f.read()
    if tag in src:
        print("SKIP (already patched): %s" % path)
        return
    n = src.count(anchor)
    if n != 1:
        print("ERROR: anchor count %d (want 1) in %s" % (n, path))
        print("anchor=<<<%s>>>" % anchor)
        sys.exit(1)
    if not os.path.exists(full + ".pktl.orig"):
        shutil.copy(full, full + ".pktl.orig")
    src = src.replace(anchor, anchor + insert, 1)
    with open(full, "w") as f:
        f.write(src)
    print("OK: %s" % path)

# ---- A. net_pkt.c : terminal-free trace, filtered to our two types ----------
patch(
    "/subsys/net/ip/net_pkt.c",
    "\t\t    net_pkt_allocs[i].alloc_data == alloc_data) {\n\t\t\tnet_pkt_allocs[i].func_free = func;\n",
    """#ifdef CONFIG_LLSS_PKT_LIFETIME
			{
				const char *fa = net_pkt_allocs[i].func_alloc;

				if (net_pkt_allocs[i].is_pkt && fa &&
				    (!strncmp(fa, "net_ipv6_send_ns", 16) ||
				     (!strncmp(fa, "eth_esp32_rx", 12) &&
				      net_pkt_get_len((struct net_pkt *)alloc_data) < 128))) {
					printk("PKTL free %p a=%s:%d by=%s:%d\\n",
					       alloc_data, fa,
					       net_pkt_allocs[i].line_alloc, func, line);
				}
			}
#endif
""",
    "PKTL free",
)

# ---- B. ipv6_nbr.c : NS alloc + send-ok + drop path -------------------------
patch(
    "/subsys/net/ip/ipv6_nbr.c",
    "\tif (!pkt) {\n\t\tret = -ENOMEM;\n\t\tgoto drop;\n\t}\n",
    """#ifdef CONFIG_LLSS_PKT_LIFETIME
	printk("PKTL ns-alloc %p t=%lld\\n", pkt, k_uptime_get());
#endif
""",
    "PKTL ns-alloc",
)
patch(
    "/subsys/net/ip/ipv6_nbr.c",
    "\tnet_ipv6_nbr_unlock();\n\n\tnet_stats_update_icmp_sent(iface);\n\tnet_stats_update_ipv6_nd_sent(iface);\n",
    """#ifdef CONFIG_LLSS_PKT_LIFETIME
	printk("PKTL ns-sentok %p\\n", pkt);
#endif
""",
    "PKTL ns-sentok",
)
patch(
    "/subsys/net/ip/ipv6_nbr.c",
    "\tif (pending != NULL) {\n\t\tnet_pkt_unref(pending);\n\t}\n\n\tif (pkt) {\n\t\tnet_pkt_unref(pkt);\n\t}\n",
    """#ifdef CONFIG_LLSS_PKT_LIFETIME
	printk("PKTL ns-drop %p ret=%d\\n", pkt, ret);
#endif
""",
    "PKTL ns-drop",
)

# ---- C. esp_wifi_drv.c : small (<128B) RX alloc trace (close_notify lives here)
patch(
    "/drivers/wifi/esp32/src/esp_wifi_drv.c",
    '\tpkt = net_pkt_rx_alloc_with_buffer(esp32_wifi_iface, len, NET_AF_UNSPEC, 0, K_MSEC(100));\n\tif (!pkt) {\n\t\tLOG_ERR("Failed to allocate net buffer");\n\t\tesp_wifi_internal_free_rx_buffer(eb);\n\t\treturn -EIO;\n\t}\n',
    """#ifdef CONFIG_LLSS_PKT_LIFETIME
	if (len < 128) {
		printk("PKTL rx-alloc %p len=%u t=%lld\\n", pkt, len, k_uptime_get());
	}
#endif
""",
    "PKTL rx-alloc",
)

print("ALL PATCHES APPLIED")
