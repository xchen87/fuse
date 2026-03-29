/*
 * platform/linux/platform.c — Linux POSIX platform implementation for FUSE
 *
 * Provides monotonic timestamps via clock_gettime(CLOCK_MONOTONIC), CPU quota
 * enforcement via SIGALRM + setitimer, and microsecond sleep via usleep.
 *
 * This is the canonical Linux platform implementation extracted from the demo
 * host applications.  The SIGALRM/sigprocmask handling is kept verbatim to
 * preserve the careful signal-safety properties of the original code.
 *
 * _GNU_SOURCE is defined by the CMake target to enable POSIX + GNU extensions
 * (clock_gettime, sigaction, sigprocmask, usleep).
 */

#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../platform.h"
#include "fuse.h"

/* ---------------------------------------------------------------------------
 * Quota state
 *
 * g_quota_module_id — the module ID to pass to fuse_quota_expired() when
 * SIGALRM fires.  Written as sig_atomic_t so reads and writes from the signal
 * handler are safe without additional locking.  -1 means no quota is armed.
 * --------------------------------------------------------------------------- */
static volatile sig_atomic_t g_quota_module_id = -1;

/* ---------------------------------------------------------------------------
 * SIGALRM handler — called from signal context (async-signal-safe path only).
 *
 * fuse_quota_expired() only calls wasm_runtime_terminate() + an atomic fence,
 * both of which are async-signal-safe by FUSE design contract.
 * --------------------------------------------------------------------------- */
static void sigalrm_handler(int sig)
{
    (void)sig;
    if (g_quota_module_id >= 0) {
        fuse_quota_expired((fuse_module_id_t)(unsigned int)g_quota_module_id);
    }
}

/* ---------------------------------------------------------------------------
 * fuse_platform_init — register SIGALRM handler.
 *
 * SA_RESTART is intentionally NOT set so that wasm_runtime_call_wasm() is
 * interrupted by the signal rather than automatically restarted, which is the
 * mechanism by which quota expiry terminates a running module step.
 * --------------------------------------------------------------------------- */
void fuse_platform_init(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigalrm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: WAMR call must be interrupted */
    sigaction(SIGALRM, &sa, NULL);
}

/* ---------------------------------------------------------------------------
 * fuse_platform_get_timestamp_us — monotonic microsecond counter.
 * --------------------------------------------------------------------------- */
uint64_t fuse_platform_get_timestamp_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000u);
}

/* ---------------------------------------------------------------------------
 * fuse_platform_quota_arm — arm a one-shot SIGALRM timer.
 *
 * The module ID is written before setitimer to ensure it is visible to the
 * signal handler even on weakly-ordered architectures.  The SEQ_CST fence
 * prevents the CPU and compiler from reordering the store across the syscall.
 * --------------------------------------------------------------------------- */
void fuse_platform_quota_arm(fuse_module_id_t mid, uint32_t quota_us)
{
    /* Write module ID before arming the timer so the signal handler always
     * sees a valid ID even if SIGALRM fires immediately after setitimer. */
    g_quota_module_id = (sig_atomic_t)mid;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    struct itimerval itv;
    memset(&itv, 0, sizeof(itv));
    itv.it_value.tv_sec  = (time_t)(quota_us / 1000000u);
    itv.it_value.tv_usec = (suseconds_t)(quota_us % 1000000u);
    setitimer(ITIMER_REAL, &itv, NULL);
}

/* ---------------------------------------------------------------------------
 * fuse_platform_quota_cancel — disarm the SIGALRM timer.
 *
 * The timer is disarmed first, then SIGALRM is blocked while the module ID
 * is cleared.  This closes the race window where the signal fires after
 * setitimer disarms but before g_quota_module_id is reset to -1, which
 * could otherwise cause fuse_quota_expired() to be called spuriously at the
 * start of the next module step.
 * --------------------------------------------------------------------------- */
void fuse_platform_quota_cancel(fuse_module_id_t mid)
{
    (void)mid;

    /* Disarm the timer first. */
    struct itimerval zero;
    memset(&zero, 0, sizeof(zero));
    setitimer(ITIMER_REAL, &zero, NULL);

    /* Block SIGALRM while clearing the module ID to close the race window
     * between disarm and ID reset. */
    sigset_t mask, old_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGALRM);
    sigprocmask(SIG_BLOCK, &mask, &old_mask);
    g_quota_module_id = -1;
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
}

/* ---------------------------------------------------------------------------
 * fuse_platform_sleep_us — sleep approximately us microseconds.
 * --------------------------------------------------------------------------- */
void fuse_platform_sleep_us(uint32_t us)
{
    usleep((useconds_t)us);
}
