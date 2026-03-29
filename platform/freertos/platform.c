/*
 * platform/freertos/platform.c — FreeRTOS platform implementation for FUSE
 *
 * NOTE: FreeRTOS software timers have tick-rate granularity (1 ms at
 * configTICK_RATE_HZ=1000).  CPU quotas shorter than one tick will round up
 * to 1 tick.  For sub-millisecond quota precision, use a hardware timer
 * peripheral and arm it from a higher-priority ISR.
 *
 * The quota timer is a one-shot FreeRTOS software timer created in
 * fuse_platform_init().  On expiry it calls fuse_quota_expired(), which is
 * designed to be called from interrupt/timer-callback context (ISR-safe).
 *
 * Timestamp resolution is limited to the FreeRTOS tick period (~1 ms at
 * configTICK_RATE_HZ=1000).  For finer-grained timestamps on real hardware,
 * replace fuse_platform_get_timestamp_us() with a hardware cycle counter.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#include "../platform.h"

/* ---------------------------------------------------------------------------
 * Quota timer state — no dynamic allocation after init.
 * --------------------------------------------------------------------------- */
static TimerHandle_t             s_quota_timer     = NULL;
static volatile fuse_module_id_t s_quota_module_id;

/* ---------------------------------------------------------------------------
 * quota_timer_cb — FreeRTOS software timer callback; called from timer task.
 *
 * fuse_quota_expired() is ISR-safe (only calls wasm_runtime_terminate() and
 * an atomic fence) and is safe to call from FreeRTOS timer task context.
 * --------------------------------------------------------------------------- */
static void quota_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    fuse_module_id_t mid = s_quota_module_id;
    if (mid != FUSE_INVALID_MODULE_ID) {
        fuse_quota_expired(mid);
    }
}

/* ---------------------------------------------------------------------------
 * fuse_platform_init — create the quota software timer.
 *
 * Must be called before fuse_platform_quota_arm().  Safe to call multiple
 * times; re-initialisation after the first call is a no-op if the timer
 * handle is already valid.
 * --------------------------------------------------------------------------- */
void fuse_platform_init(void)
{
    s_quota_module_id = FUSE_INVALID_MODULE_ID;
    if (s_quota_timer == NULL) {
        /* Period of 1 tick — will be reset by xTimerChangePeriod() in
         * fuse_platform_quota_arm() before the timer is started. */
        s_quota_timer = xTimerCreate(
            "fuse_quota",
            (TickType_t)1,
            pdFALSE,      /* auto-reload: off (one-shot) */
            NULL,
            quota_timer_cb
        );
    }
}

/* ---------------------------------------------------------------------------
 * fuse_platform_get_timestamp_us — monotonic tick-based microsecond counter.
 *
 * Resolution is one FreeRTOS tick (1 ms at configTICK_RATE_HZ=1000).
 * --------------------------------------------------------------------------- */
uint64_t fuse_platform_get_timestamp_us(void)
{
    TickType_t ticks = xTaskGetTickCount();
    return (uint64_t)ticks * (1000000ULL / (uint64_t)configTICK_RATE_HZ);
}

/* ---------------------------------------------------------------------------
 * fuse_platform_quota_arm — arm the one-shot quota software timer.
 *
 * Converts quota_us to ticks, clamping to a minimum of 1 tick to prevent
 * immediate expiry before the guarded WAMR call begins.
 * --------------------------------------------------------------------------- */
void fuse_platform_quota_arm(fuse_module_id_t mid, uint32_t quota_us)
{
    if (s_quota_timer == NULL) {
        return;
    }
    s_quota_module_id = mid;

    /* Convert µs to ticks; minimum 1 tick to avoid zero-period timer. */
    TickType_t ticks = (TickType_t)(quota_us /
        (1000000UL / (uint32_t)configTICK_RATE_HZ));
    if (ticks == 0u) {
        ticks = (TickType_t)1;
    }

    xTimerChangePeriod(s_quota_timer, ticks, (TickType_t)0);
    xTimerStart(s_quota_timer, (TickType_t)0);
}

/* ---------------------------------------------------------------------------
 * fuse_platform_quota_cancel — stop the quota software timer.
 * --------------------------------------------------------------------------- */
void fuse_platform_quota_cancel(fuse_module_id_t mid)
{
    (void)mid;
    if (s_quota_timer == NULL) {
        return;
    }
    xTimerStop(s_quota_timer, (TickType_t)0);
    s_quota_module_id = FUSE_INVALID_MODULE_ID;
}

/* ---------------------------------------------------------------------------
 * fuse_platform_sleep_us — delay by approximately us microseconds.
 *
 * Rounds up to the nearest tick boundary.  Minimum delay is 1 tick.
 * --------------------------------------------------------------------------- */
void fuse_platform_sleep_us(uint32_t us)
{
    TickType_t ticks = pdMS_TO_TICKS(us / 1000u);
    if (ticks == (TickType_t)0) {
        ticks = (TickType_t)1;
    }
    vTaskDelay(ticks);
}
