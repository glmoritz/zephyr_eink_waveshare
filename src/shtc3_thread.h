#ifndef SHTC3_THREAD_H_
#define SHTC3_THREAD_H_

#include <stdbool.h>
#include <stdint.h>

enum shtc3_data_status {
	SHTC3_DATA_PENDING = 0,
	SHTC3_DATA_VALID,
	SHTC3_DATA_UNAVAILABLE,
	SHTC3_DATA_ERROR,
	SHTC3_DATA_SIMULATED,
};

struct shtc3_data {
	enum shtc3_data_status status;
	int32_t temperature_centi_c;
	int32_t humidity_centi_pct;
	int32_t age_ms;
	int64_t sample_time_ms;
	int32_t last_error;
	bool stale;
	bool has_sample;
};

bool shtc3_get_latest(struct shtc3_data *out);

#endif /* SHTC3_THREAD_H_ */