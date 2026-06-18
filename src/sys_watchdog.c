#include "sys_watchdog.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sys_wdt, LOG_LEVEL_INF);

#if defined(CONFIG_LLSS_HW_WATCHDOG)

#include <zephyr/sys/reboot.h>

#if !defined(CONFIG_LLSS_HW_WATCHDOG_DRY_RUN)
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#define HW_WDT_NODE DT_NODELABEL(wdt0)
#endif

#define WDT_MAX_TASKS   6
#define WDT_SUP_STACK   1024
#define WDT_SUP_PRIO    5     /* above the app threads so it preempts and runs */

struct wtask {
	const char *name;
	uint32_t    timeout_ms;
	bool        used;
	bool        active;      /* false = intentionally idle, ignored */
	int64_t     last_alive_ms;
};

static struct wtask   tasks[WDT_MAX_TASKS];
static struct k_spinlock lock;

int sys_wdt_register(const char *name, uint32_t timeout_ms)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int id = -1;

	for (int i = 0; i < WDT_MAX_TASKS; i++) {
		if (!tasks[i].used) {
			tasks[i].used = true;
			tasks[i].name = name;
			tasks[i].timeout_ms = timeout_ms;
			tasks[i].active = true;
			tasks[i].last_alive_ms = k_uptime_get();
			id = i;
			break;
		}
	}
	k_spin_unlock(&lock, key);

	if (id < 0) {
		LOG_ERR("watchdog table full — '%s' not registered", name);
	} else {
		LOG_INF("watchdog ch %d = '%s' (timeout %u ms)", id, name, timeout_ms);
	}
	return id;
}

void sys_wdt_alive(int handle)
{
	if (handle < 0 || handle >= WDT_MAX_TASKS) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);

	tasks[handle].active = true;
	tasks[handle].last_alive_ms = k_uptime_get();
	k_spin_unlock(&lock, key);
}

void sys_wdt_idle(int handle)
{
	if (handle < 0 || handle >= WDT_MAX_TASKS) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);

	tasks[handle].active = false;
	k_spin_unlock(&lock, key);
}

#if !defined(CONFIG_LLSS_HW_WATCHDOG_DRY_RUN)
/* Arm the ESP32 hardware WDT. Returns the channel id, or <0 on failure. */
static int hw_wdt_arm(const struct device **dev_out)
{
	const struct device *dev = DEVICE_DT_GET(HW_WDT_NODE);

	if (!device_is_ready(dev)) {
		LOG_ERR("HW WDT not ready — running software-only");
		return -ENODEV;
	}

	struct wdt_timeout_cfg cfg = {
		.window = { .min = 0U,
			    .max = CONFIG_LLSS_HW_WATCHDOG_HW_TIMEOUT_MS },
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	int ch = wdt_install_timeout(dev, &cfg);

	if (ch < 0) {
		LOG_ERR("wdt_install_timeout: %d", ch);
		return ch;
	}
	int rc = wdt_setup(dev, WDT_OPT_PAUSE_HALTED_BY_DBG);

	if (rc < 0) {
		LOG_ERR("wdt_setup: %d", rc);
		return rc;
	}
	*dev_out = dev;
	LOG_INF("HW WDT armed (%d ms)", CONFIG_LLSS_HW_WATCHDOG_HW_TIMEOUT_MS);
	return ch;
}
#endif /* !DRY_RUN */

/* Returns the index of the first hung (active & overdue) task, or -1. */
static int find_hung(int64_t now, int64_t *overdue_ms_out)
{
	int hung = -1;
	k_spinlock_key_t key = k_spin_lock(&lock);

	for (int i = 0; i < WDT_MAX_TASKS; i++) {
		if (tasks[i].used && tasks[i].active) {
			int64_t idle = now - tasks[i].last_alive_ms;

			if (idle > (int64_t)tasks[i].timeout_ms) {
				hung = i;
				*overdue_ms_out = idle;
				break;
			}
		}
	}
	k_spin_unlock(&lock, key);
	return hung;
}

static void sys_wdt_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

#if !defined(CONFIG_LLSS_HW_WATCHDOG_DRY_RUN)
	const struct device *hw_dev = NULL;
	int hw_ch = hw_wdt_arm(&hw_dev);
#endif

	LOG_INF("system watchdog supervisor up (%s, check %d ms)",
		IS_ENABLED(CONFIG_LLSS_HW_WATCHDOG_DRY_RUN)
			? "DRY-RUN, no reset" : "HW-backed",
		CONFIG_LLSS_HW_WATCHDOG_CHECK_MS);

	for (;;) {
		k_msleep(CONFIG_LLSS_HW_WATCHDOG_CHECK_MS);

		int64_t overdue_ms = 0;
		int hung = find_hung(k_uptime_get(), &overdue_ms);

		if (hung >= 0) {
#if defined(CONFIG_LLSS_HW_WATCHDOG_DRY_RUN)
			LOG_ERR("WATCHDOG: would reset now — '%s' starved (%lld ms; timeout %u)",
				tasks[hung].name, (long long)overdue_ms,
				tasks[hung].timeout_ms);
			/* dry-run: keep running, do not reset. */
#else
			LOG_ERR("WATCHDOG: '%s' starved (%lld ms) — resetting",
				tasks[hung].name, (long long)overdue_ms);
			k_msleep(200); /* flush the log */
			sys_reboot(SYS_REBOOT_COLD);
			/* if sys_reboot somehow returns, stop feeding -> HW resets */
			continue;
#endif
		}

#if !defined(CONFIG_LLSS_HW_WATCHDOG_DRY_RUN)
		/* Healthy this cycle — feed the HW WDT. If we ever stop looping
		 * (supervisor hung) or detect a starvation above, the HW WDT is no
		 * longer fed and fires. */
		if (hw_dev != NULL && hung < 0) {
			wdt_feed(hw_dev, hw_ch);
		}
#endif
	}
}

K_THREAD_DEFINE(sys_wdt_tid, WDT_SUP_STACK, sys_wdt_thread,
		NULL, NULL, NULL, WDT_SUP_PRIO, 0, 0);

/* Idle sentinel — lowest application priority. It just heartbeats on a short
 * sleep, so it only runs when the CPU has drained all higher-priority work. If
 * ANY thread monopolises the CPU (a hot loop) at a priority above this, the
 * sentinel can't be scheduled, goes stale, and the supervisor catches it — so a
 * CPU-eating runaway is caught even in threads that aren't individually
 * instrumented. (A pure deadlock that doesn't hog the CPU is NOT caught here —
 * the system still reaches idle; functional liveness of a specific thread must
 * be covered by that thread's own beat.) Timeout is generous so legitimate
 * CPU-bound bursts — TLS ECC handshake, LVGL render — don't false-trip. */
#define WDT_SENTINEL_STACK      768
#define WDT_SENTINEL_PRIO       K_LOWEST_APPLICATION_THREAD_PRIO
#define WDT_SENTINEL_SLEEP_MS   1000
#define WDT_SENTINEL_TIMEOUT_MS 30000

static void wdt_sentinel_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	int h = sys_wdt_register("idle-sentinel", WDT_SENTINEL_TIMEOUT_MS);

	for (;;) {
		sys_wdt_alive(h);
		k_msleep(WDT_SENTINEL_SLEEP_MS);
	}
}

K_THREAD_DEFINE(wdt_sentinel_tid, WDT_SENTINEL_STACK, wdt_sentinel_thread,
		NULL, NULL, NULL, WDT_SENTINEL_PRIO, 0, 0);

#else /* !CONFIG_LLSS_HW_WATCHDOG */

int sys_wdt_register(const char *name, uint32_t timeout_ms)
{
	ARG_UNUSED(name);
	ARG_UNUSED(timeout_ms);
	return -1;
}
void sys_wdt_alive(int handle) { ARG_UNUSED(handle); }
void sys_wdt_idle(int handle)  { ARG_UNUSED(handle); }

#endif /* CONFIG_LLSS_HW_WATCHDOG */
