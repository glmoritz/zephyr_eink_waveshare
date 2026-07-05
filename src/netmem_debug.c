/*
 * netmem_debug.c — temporary on-demand resource telemetry.
 *
 * Chasing the slow leak that panics the real ESP32-S3 after hours with an
 * "esp32_wifi: unable to allocate net buffer" storm followed by a load-prohibited
 * fault in llss_thread.
 *
 * The failing allocation is net_pkt_rx_alloc_with_buffer() in the esp32 WiFi
 * driver (drivers/wifi/esp32/src/esp_wifi_drv.c:153) — i.e. the Zephyr RX
 * net_pkt slab + RX net_buf data pool. But shrinking those pools did NOT shorten
 * time-to-crash, so the binding resource may be elsewhere (the IDF WiFi heap in
 * PSRAM, etc.). This module exposes shell commands so the operator can print a
 * greppable "NETMEM" line only when needed instead of flooding the console.
 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/sys/mem_stats.h>
#if defined(CONFIG_THREAD_ANALYZER)
#include <zephyr/debug/thread_analyzer.h>
#endif

/* Zephyr system heap (k_malloc/k_free target). On this ESP32-S3 build the WiFi
 * driver's big buffers go to the PSRAM shared_multi_heap (ESP_WIFI_HEAP_SPIRAM),
 * which exposes no public free/stats API — but plenty still lands here
 * (net_context/socket/queue allocations, etc.), and the IDF heap_caps_*() shim
 * returns 0 on this port, so the system heap is the queryable heap signal.
 * The crash-determining resource (the RX net_pkt/net_buf pool) is tracked above
 * regardless. */
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS) && (CONFIG_HEAP_MEM_POOL_SIZE > 0)
extern struct k_heap _system_heap;
#define HAVE_SYS_HEAP_STATS 1
#endif

#if defined(CONFIG_NET_BUF_POOL_USAGE)
#define POOL_AVAIL(p) ((long)atomic_get(&(p)->avail_count))
#define POOL_MAX(p)   ((unsigned int)(p)->max_used)
#else
/* Without NET_BUF_POOL_USAGE avail/max aren't tracked — report -1/0. */
#define POOL_AVAIL(p) (-1L)
#define POOL_MAX(p)   (0u)
#endif

static void netmem_dump(const struct shell *sh)
{
	struct k_mem_slab   *rx = NULL, *tx = NULL;
	struct net_buf_pool *rx_data = NULL, *tx_data = NULL;

	net_pkt_get_info(&rx, &tx, &rx_data, &tx_data);

	unsigned int pkt_rx_free  = rx ? (unsigned int)k_mem_slab_num_free_get(rx) : 0;
	unsigned int pkt_rx_total = rx ? (unsigned int)rx->info.num_blocks : 0;
	unsigned int pkt_tx_free  = tx ? (unsigned int)k_mem_slab_num_free_get(tx) : 0;
	unsigned int pkt_tx_total = tx ? (unsigned int)tx->info.num_blocks : 0;

	long         buf_rx_avail = rx_data ? POOL_AVAIL(rx_data) : -1L;
	unsigned int buf_rx_total = rx_data ? (unsigned int)rx_data->buf_count : 0;
	unsigned int buf_rx_max   = rx_data ? POOL_MAX(rx_data) : 0;
	long         buf_tx_avail = tx_data ? POOL_AVAIL(tx_data) : -1L;
	unsigned int buf_tx_total = tx_data ? (unsigned int)tx_data->buf_count : 0;
	unsigned int buf_tx_max   = tx_data ? POOL_MAX(tx_data) : 0;

	unsigned long long up_s = (unsigned long long)(k_uptime_get() / 1000);

	size_t sh_free = 0, sh_alloc = 0, sh_max = 0;
#ifdef HAVE_SYS_HEAP_STATS
	struct sys_memory_stats st;

	if (sys_heap_runtime_stats_get(&_system_heap.heap, &st) == 0) {
		sh_free  = st.free_bytes;
		sh_alloc = st.allocated_bytes;
		sh_max   = st.max_allocated_bytes;
	}
#endif

	/* One greppable line. Watch the trend over hours:
	 *   pktRX/pktTX  = net_pkt slab free/total      (RX is the crash resource)
	 *   bufRX/bufTX  = net_buf data pool avail/total/maxUsed
	 *   sysheap      = Zephyr k_malloc heap free/allocated/maxAllocated
	 * A column whose free/avail floor ratchets downward across snapshots is the
	 * leak; sysheap_max climbing without bound is a k_malloc leak. */
	shell_print(sh,
		    "NETMEM up=%llus pktRX=%u/%u pktTX=%u/%u "
		    "bufRX=%ld/%u/m%u bufTX=%ld/%u/m%u "
		    "sysheap_free=%zu alloc=%zu max=%zu",
		    up_s, pkt_rx_free, pkt_rx_total, pkt_tx_free, pkt_tx_total,
		    buf_rx_avail, buf_rx_total, buf_rx_max,
		    buf_tx_avail, buf_tx_total, buf_tx_max,
		    sh_free, sh_alloc, sh_max);
}

static int cmd_diag_netmem(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	netmem_dump(sh);

	return 0;
}

#if defined(CONFIG_THREAD_ANALYZER)
static int cmd_diag_threads(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	thread_analyzer_print(0);

	return 0;
}

static int cmd_diag_all(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	netmem_dump(sh);
	thread_analyzer_print(0);

	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(sub_diag,
	SHELL_CMD(netmem, NULL,
		  "Print one-shot network/buffer/heap telemetry", cmd_diag_netmem),
#if defined(CONFIG_THREAD_ANALYZER)
	SHELL_CMD(threads, NULL,
		  "Print one-shot thread stack/runtime telemetry", cmd_diag_threads),
	SHELL_CMD(all, NULL,
		  "Print one-shot NETMEM + thread telemetry", cmd_diag_all),
#endif
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(diag, &sub_diag,
		   "On-demand debug telemetry helpers", NULL);
