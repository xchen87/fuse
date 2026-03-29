/*
 * platform/platform.h — FUSE platform portability interface
 *
 * Common interface implemented by each platform target.  Include this header
 * in host application code and wire the functions into fuse_hal_t before
 * calling fuse_init().
 *
 * Supported platform implementations:
 *   platform/linux/    — POSIX/Linux: clock_gettime + SIGALRM + setitimer
 *   platform/freertos/ — FreeRTOS POSIX simulator: tick counter + software timers
 */

#ifndef FUSE_PLATFORM_H
#define FUSE_PLATFORM_H

#include <stdint.h>
#include "fuse.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * fuse_platform_init — initialise platform resources.
 *
 * On Linux: registers the SIGALRM handler for quota enforcement.
 * On FreeRTOS: creates the quota software timer handle.
 *
 * Must be called exactly once before any other fuse_platform_* function and
 * before fuse_init().
 */
void fuse_platform_init(void);

/*
 * fuse_platform_get_timestamp_us — monotonic microsecond counter.
 *
 * Returns the number of microseconds elapsed since an arbitrary epoch
 * (implementation-defined; monotonic and non-decreasing).  Suitable for
 * wiring into hal.timer.get_timestamp.
 */
uint64_t fuse_platform_get_timestamp_us(void);

/*
 * fuse_platform_quota_arm — arm a one-shot CPU quota timer.
 *
 * Arms the platform timer so that fuse_quota_expired(mid) is called after
 * quota_us microseconds if the timer is not cancelled first.  Suitable for
 * wiring into hal.quota_arm.
 *
 * mid      — module ID to pass to fuse_quota_expired() on expiry.
 * quota_us — quota duration in microseconds.  0 disarms the timer without
 *            starting a new one.
 */
void fuse_platform_quota_arm(fuse_module_id_t mid, uint32_t quota_us);

/*
 * fuse_platform_quota_cancel — cancel the armed quota timer.
 *
 * Cancels the timer armed by fuse_platform_quota_arm().  Safe to call when
 * no timer is armed.  Suitable for wiring into hal.quota_cancel.
 *
 * mid — module ID (informational; used for bookkeeping if needed).
 */
void fuse_platform_quota_cancel(fuse_module_id_t mid);

/*
 * fuse_platform_sleep_us — sleep approximately us microseconds.
 *
 * Used in the demo poll loop between fuse_tick() calls.  The actual sleep
 * duration is platform-dependent and may be rounded up to the next timer
 * tick boundary.
 */
void fuse_platform_sleep_us(uint32_t us);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_PLATFORM_H */
