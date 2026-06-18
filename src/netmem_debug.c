/*
 * netmem_debug.c — temporary resource-leak telemetry (CONFIG_LLSS_NETMEM_DEBUG).
 *
 * Chasing the slow leak that panics the real ESP32-S3 after hours with an
 * "esp32_wifi: unable to allocate net buffer" storm followed by a load-prohibited
 * fault in llss_thread (see memory project_esp32_netbuf_crash).
 *
 * The failing allocation is net_pkt_rx_alloc_with_buffer() in the esp32 WiFi
 * driver (drivers/wifi/esp32/src/esp_wifi_drv.c:153) — i.e. the Zephyr RX
 * net_pkt slab + RX net_buf data pool. But shrinking those pools did NOT shorten
 * time-to-crash, so the binding resource may be elsewhere (the IDF WiFi heap in
 * PSRAM, etc.). This thread prints ALL of them on one greppable line so the
 * boot->crash capture shows which one trends to zero, and at what rate.
 *
 * Single line per tick (default 30 s), via printk (synchronous — survives the
 * pre-crash log congestion that can drop LOG_* messages). grep 'NETMEM' on the
 * captured serial log; plot the falling column.
 *
 * Pair with CONFIG_THREAD_ANALYZER_AUTO (per-thread stack high-water) and the
 * net debug logs in debug.conf. Remove (drop CONFIG_LLSS_NETMEM_DEBUG) once the
 * leak is found and fixed.
 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/sys/mem_stats.h>

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

#define NETMEM_STACK_SIZE 1280
#define NETMEM_PRIORITY   14 /* below llss_thread (10) and display */

#if defined(CONFIG_NET_BUF_POOL_USAGE)
#define POOL_AVAIL(p) ((long)atomic_get(&(p)->avail_count))
#define POOL_MAX(p)   ((unsigned int)(p)->max_used)
#else
/* Without NET_BUF_POOL_USAGE avail/max aren't tracked — report -1/0. */
#define POOL_AVAIL(p) (-1L)
#define POOL_MAX(p)   (0u)
#endif

static void netmem_dump(void)
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
	 * A column whose free/avail floor ratchets downward across cycles is the
	 * leak; sysheap_max climbing without bound is a k_malloc leak. */
	printk("NETMEM up=%llus pktRX=%u/%u pktTX=%u/%u "
	       "bufRX=%ld/%u/m%u bufTX=%ld/%u/m%u "
	       "sysheap_free=%zu alloc=%zu max=%zu\n",
	       up_s, pkt_rx_free, pkt_rx_total, pkt_tx_free, pkt_tx_total,
	       buf_rx_avail, buf_rx_total, buf_rx_max,
	       buf_tx_avail, buf_tx_total, buf_tx_max,
	       sh_free, sh_alloc, sh_max);
}

static void netmem_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		netmem_dump();
		k_msleep(CONFIG_LLSS_NETMEM_DEBUG_INTERVAL_MS);
	}
}

K_THREAD_DEFINE(netmem_tid, NETMEM_STACK_SIZE, netmem_thread,
		NULL, NULL, NULL, NETMEM_PRIORITY, 0, 0);
