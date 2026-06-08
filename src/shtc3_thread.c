#include "shtc3_thread.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "system_flags.h"

LOG_MODULE_REGISTER(shtc3, LOG_LEVEL_INF);

#define SHTC3_THREAD_STACK         2048
#define SHTC3_THREAD_PRIORITY      11
#define SHTC3_SAMPLE_INTERVAL_MS   (60 * MSEC_PER_SEC)
#define SHTC3_STALE_AFTER_MS       (90 * MSEC_PER_SEC)
#define SHTC3_UNAVAILABLE_RETRY_MS (5 * 60 * MSEC_PER_SEC)

enum shtc3_state {
	STATE_BOOTING = 0,
	STATE_WAITING_DEVICE,
	STATE_SAMPLING,
	STATE_SLEEPING,
	STATE_SIMULATED,
};

static enum shtc3_state shtc3_state = STATE_BOOTING;
static K_MUTEX_DEFINE(shtc3_data_mutex);

static struct shtc3_data latest_data = {
	.status = SHTC3_DATA_PENDING,
	.sample_time_ms = -1,
	.age_ms = -1,
	.last_error = 0,
};

#if DT_HAS_COMPAT_STATUS_OKAY(sensirion_shtcx)
static const struct device *const shtc3_dev = DEVICE_DT_GET_ANY(sensirion_shtcx);
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(sensirion_shtcx) || !defined(CONFIG_BOARD_NATIVE_SIM)
static void publish_status(enum shtc3_data_status status, int32_t error)
{
	k_mutex_lock(&shtc3_data_mutex, K_FOREVER);
	latest_data.status = status;
	latest_data.last_error = error;
	k_mutex_unlock(&shtc3_data_mutex);
}
#endif

static void publish_sample(enum shtc3_data_status status,
			       int32_t temperature_centi_c,
			       int32_t humidity_centi_pct)
{
	k_mutex_lock(&shtc3_data_mutex, K_FOREVER);
	latest_data.status = status;
	latest_data.temperature_centi_c = temperature_centi_c;
	latest_data.humidity_centi_pct = humidity_centi_pct;
	latest_data.sample_time_ms = k_uptime_get();
	latest_data.age_ms = 0;
	latest_data.last_error = 0;
	latest_data.stale = false;
	latest_data.has_sample = true;
	k_mutex_unlock(&shtc3_data_mutex);
}

bool shtc3_get_latest(struct shtc3_data *out)
{
	int32_t rc;
	int64_t age_ms;

	if (out == NULL) {
		return false;
	}

	rc = k_mutex_lock(&shtc3_data_mutex, K_NO_WAIT);
	if (rc != 0) {
		return false;
	}

	*out = latest_data;
	k_mutex_unlock(&shtc3_data_mutex);

	if (!out->has_sample || out->sample_time_ms < 0) {
		out->age_ms = -1;
		out->stale = true;
		return true;
	}

	age_ms = k_uptime_get() - out->sample_time_ms;
	if (age_ms < 0) {
		age_ms = 0;
	}

	out->age_ms = (int32_t)MIN(age_ms, INT32_MAX);
	out->stale = age_ms > SHTC3_STALE_AFTER_MS;
	return true;
}

#if DT_HAS_COMPAT_STATUS_OKAY(sensirion_shtcx)
static bool shtc3_ready(void)
{
	return device_is_ready(shtc3_dev);
}

static int shtc3_read_once(void)
{
	struct sensor_value temperature;
	struct sensor_value humidity;
	int32_t rc;
	int64_t temp_centi;
	int64_t humidity_centi;

	sys_io_acquire();
	rc = sensor_sample_fetch(shtc3_dev);
	if (rc == 0) {
		rc = sensor_channel_get(shtc3_dev, SENSOR_CHAN_AMBIENT_TEMP,
				       &temperature);
	}
	if (rc == 0) {
		rc = sensor_channel_get(shtc3_dev, SENSOR_CHAN_HUMIDITY,
				       &humidity);
	}
	sys_io_release();

	if (rc != 0) {
		publish_status(SHTC3_DATA_ERROR, rc);
		LOG_WRN("SHTC3 sample failed: %d", rc);
		return rc;
	}

	temp_centi = sensor_value_to_centi(&temperature);
	humidity_centi = sensor_value_to_centi(&humidity);
	publish_sample(SHTC3_DATA_VALID, (int32_t)temp_centi,
		       (int32_t)humidity_centi);

	LOG_INF("SHTC3: temperature=%d.%02d C humidity=%d.%02d %%",
		(int32_t)(temp_centi / 100), (int32_t)llabs(temp_centi % 100),
		(int32_t)(humidity_centi / 100), (int32_t)llabs(humidity_centi % 100));

	return 0;
}
#endif

static enum shtc3_state do_boot(void)
{
#if DT_HAS_COMPAT_STATUS_OKAY(sensirion_shtcx)
	if (shtc3_ready()) {
		LOG_INF("SHTC3 backend ready");
		return STATE_SAMPLING;
	}

	publish_status(SHTC3_DATA_UNAVAILABLE, -ENODEV);
	LOG_WRN("SHTC3 backend not ready; retry in %d s",
		(int32_t)(SHTC3_UNAVAILABLE_RETRY_MS / MSEC_PER_SEC));
	return STATE_WAITING_DEVICE;
#elif defined(CONFIG_BOARD_NATIVE_SIM)
	LOG_INF("SHTC3 native_sim fallback enabled");
	return STATE_SIMULATED;
#else
	publish_status(SHTC3_DATA_UNAVAILABLE, -ENODEV);
	LOG_WRN("SHTC3 not present in devicetree");
	return STATE_WAITING_DEVICE;
#endif
}

static enum shtc3_state do_wait_device(void)
{
	k_sleep(K_MSEC(SHTC3_UNAVAILABLE_RETRY_MS));
	return STATE_BOOTING;
}

static enum shtc3_state do_sleep(void)
{
	k_sleep(K_MSEC(SHTC3_SAMPLE_INTERVAL_MS));
	return STATE_SAMPLING;
}

#if defined(CONFIG_BOARD_NATIVE_SIM)
static enum shtc3_state do_simulated(void)
{
	int64_t minutes = k_uptime_get() / SHTC3_SAMPLE_INTERVAL_MS;
	int32_t temp_centi = 2150 + (int32_t)((minutes % 7) * 15);
	int32_t humidity_centi = 4800 + (int32_t)((minutes % 5) * 35);

	publish_sample(SHTC3_DATA_SIMULATED, temp_centi, humidity_centi);
	LOG_INF("SHTC3(sim): temperature=%d.%02d C humidity=%d.%02d %%",
		temp_centi / 100, (int32_t)llabs(temp_centi % 100),
		humidity_centi / 100, (int32_t)llabs(humidity_centi % 100));
	k_sleep(K_MSEC(SHTC3_SAMPLE_INTERVAL_MS));
	return STATE_SIMULATED;
}
#endif

static void shtc3_thread_fn(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		switch (shtc3_state) {
		case STATE_BOOTING:
			shtc3_state = do_boot();
			break;
		case STATE_WAITING_DEVICE:
			shtc3_state = do_wait_device();
			break;
		case STATE_SAMPLING:
#if DT_HAS_COMPAT_STATUS_OKAY(sensirion_shtcx)
			if (!shtc3_ready()) {
				publish_status(SHTC3_DATA_UNAVAILABLE, -ENODEV);
				shtc3_state = STATE_WAITING_DEVICE;
				break;
			}

			(void)shtc3_read_once();
			shtc3_state = STATE_SLEEPING;
#else
			shtc3_state = STATE_BOOTING;
#endif
			break;
		case STATE_SLEEPING:
			shtc3_state = do_sleep();
			break;
		case STATE_SIMULATED:
#if defined(CONFIG_BOARD_NATIVE_SIM)
			shtc3_state = do_simulated();
#else
			shtc3_state = STATE_BOOTING;
#endif
			break;
		default:
			shtc3_state = STATE_BOOTING;
			break;
		}
	}
}

K_THREAD_DEFINE(shtc3_thread, SHTC3_THREAD_STACK,
		shtc3_thread_fn, NULL, NULL, NULL,
		SHTC3_THREAD_PRIORITY, 0, 0);