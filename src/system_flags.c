#include "system_flags.h"

K_EVENT_DEFINE(system_flags);

struct k_poll_signal system_flags_changed =
	K_POLL_SIGNAL_INITIALIZER(system_flags_changed);

atomic_t system_io_inflight = ATOMIC_INIT(0);
