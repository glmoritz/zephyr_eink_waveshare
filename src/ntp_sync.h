#ifndef NTP_SYNC_H_
#define NTP_SYNC_H_

/*
 * The NTP subsystem is a self-contained state-machine thread that owns
 * SYS_FLAG_TIME_VALID (see system_flags.h).  It auto-starts via
 * K_THREAD_DEFINE and has no public API: consumers wait on the flag.
 *
 * This header exists only to anchor the translation unit in the build
 * (referenced from CMakeLists.txt) and to document the contract.
 */

#endif /* NTP_SYNC_H_ */
