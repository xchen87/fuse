/*
 * main.c — Camera compression demo host application
 *
 * Loads the camera_compress WASM/AOT module under FUSE, then calls
 * fuse_module_run_step() every 10 seconds for 5 iterations.
 * Each step is limited to 1 ms of wall-clock CPU time via a SIGALRM quota.
 *
 * Usage:
 *   camera_compress_host <module.aot> <policy.bin>
 */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "fuse.h"

/* ---------------------------------------------------------------------------
 * Static FUSE memory pools (no dynamic allocation for FUSE itself)
 * --------------------------------------------------------------------------- */
static uint8_t g_module_mem[8u * 1024u * 1024u];  /* 8 MB WAMR pool   */
static uint8_t g_log_mem[64u * 1024u];             /* 64 KB log ring   */

/* ---------------------------------------------------------------------------
 * Simulated camera
 * --------------------------------------------------------------------------- */
#define SIM_FRAME_SIZE (256u * 1024u)   /* 256 KB synthetic frame */

static uint8_t g_sim_frame[SIM_FRAME_SIZE];
static int     g_frame_ready = 0;

static void init_sim_frame(void)
{
    /* Gradient pattern — friendly to RLE on runs of same value */
    for (unsigned int i = 0u; i < SIM_FRAME_SIZE; ++i) {
        g_sim_frame[i] = (uint8_t)(i / 256u);  /* 256-byte constant runs */
    }
    g_frame_ready = 1;
}

/* ---------------------------------------------------------------------------
 * HAL callbacks
 * --------------------------------------------------------------------------- */

static uint64_t hal_timer(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000u);
}

static uint64_t hal_camera(void *buf, uint32_t max_len)
{
    /* buf has already been validated by fuse_native_camera_last_frame()
     * via wasm_runtime_validate_native_addr() before this callback is
     * invoked (see core/fuse_hal.c:181).  A NULL check here is a
     * defence-in-depth guard only. */
    if (buf == NULL || max_len == 0u) {
        return 0u;
    }
    if (!g_frame_ready) {
        init_sim_frame();
    }
    uint32_t copy_len = (SIM_FRAME_SIZE < max_len) ? SIM_FRAME_SIZE : max_len;
    memcpy(buf, g_sim_frame, copy_len);
    return (uint64_t)copy_len;
}

/* Quota timer uses SIGALRM + setitimer.  The signal handler may only call
 * async-signal-safe functions; fuse_quota_expired() only calls
 * wasm_runtime_terminate() and an atomic fence, which is ISR-safe. */
static volatile sig_atomic_t g_quota_module_id = -1;

static void sigalrm_handler(int sig)
{
    (void)sig;
    if (g_quota_module_id >= 0) {
        fuse_quota_expired((fuse_module_id_t)(unsigned int)g_quota_module_id);
    }
}

static void hal_quota_arm(fuse_module_id_t mid, uint32_t quota_us)
{
    /* Write the module ID before arming the timer.  The compiler/CPU fence
     * ensures this store is visible before the setitimer syscall that may
     * cause SIGALRM to fire on a weakly-ordered architecture. */
    g_quota_module_id = (sig_atomic_t)mid;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    struct itimerval itv;
    memset(&itv, 0, sizeof(itv));
    itv.it_value.tv_sec  = (time_t)(quota_us / 1000000u);
    itv.it_value.tv_usec = (suseconds_t)(quota_us % 1000000u);
    setitimer(ITIMER_REAL, &itv, NULL);
}

static void hal_quota_cancel(fuse_module_id_t mid)
{
    (void)mid;

    /* Disarm the timer first. */
    struct itimerval zero;
    memset(&zero, 0, sizeof(zero));
    setitimer(ITIMER_REAL, &zero, NULL);

    /* Block SIGALRM while clearing the module ID to close the window where
     * the signal could fire after disarm but before the ID is reset,
     * spuriously calling fuse_quota_expired() on the next step's module. */
    sigset_t mask, old_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGALRM);
    sigprocmask(SIG_BLOCK, &mask, &old_mask);
    g_quota_module_id = -1;
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
}

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

static uint8_t *read_file(const char *path, uint32_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) {
        fprintf(stderr, "error: empty or unreadable file '%s'\n", path);
        fclose(f);
        return NULL;
    }
    if ((unsigned long)sz > (unsigned long)UINT32_MAX) {
        fprintf(stderr, "error: file '%s' exceeds 4 GB\n", path);
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fprintf(stderr, "error: malloc failed for %ld bytes\n", sz);
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "error: short read on '%s'\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (uint32_t)sz;
    return buf;
}

static const char *fuse_stat_name(fuse_stat_t s)
{
    switch (s) {
        case FUSE_SUCCESS:               return "FUSE_SUCCESS";
        case FUSE_ERR_QUOTA_EXCEEDED:    return "FUSE_ERR_QUOTA_EXCEEDED";
        case FUSE_ERR_MODULE_TRAP:       return "FUSE_ERR_MODULE_TRAP";
        case FUSE_ERR_INVALID_ARG:       return "FUSE_ERR_INVALID_ARG";
        case FUSE_ERR_NOT_INITIALIZED:   return "FUSE_ERR_NOT_INITIALIZED";
        case FUSE_ERR_MODULE_NOT_FOUND:  return "FUSE_ERR_MODULE_NOT_FOUND";
        default:                          return "FUSE_ERR_OTHER";
    }
}

/* ---------------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------------- */

#define NUM_STEPS   5
#define STEP_PERIOD 10  /* seconds between steps */

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <module.aot> <policy.bin>\n"
                "  module.aot  — AOT-compiled WASM module\n"
                "  policy.bin  — binary fuse_policy_t (20 bytes)\n",
                argv[0]);
        return 1;
    }

    /* --- Register SIGALRM handler for CPU quota --- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigalrm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  /* no SA_RESTART: wasm call should be interrupted */
    if (sigaction(SIGALRM, &sa, NULL) != 0) {
        perror("sigaction");
        return 1;
    }

    /* --- Load AOT binary --- */
    uint32_t aot_size   = 0u;
    uint8_t *aot_buf    = read_file(argv[1], &aot_size);
    if (!aot_buf) return 1;

    /* --- Load policy binary --- */
    uint32_t policy_size = 0u;
    uint8_t *policy_buf  = read_file(argv[2], &policy_size);
    if (!policy_buf) { free(aot_buf); return 1; }

    if (policy_size != sizeof(fuse_policy_t)) {
        fprintf(stderr,
                "error: policy.bin is %u bytes, expected %zu\n",
                policy_size, sizeof(fuse_policy_t));
        free(aot_buf);
        free(policy_buf);
        return 1;
    }
    const fuse_policy_t *policy = (const fuse_policy_t *)policy_buf;

    printf("=== FUSE camera-compress demo ===\n");
    printf("  module : %s (%u bytes)\n", argv[1], aot_size);
    printf("  policy : capabilities=0x%x  pages=%u  quota=%u us\n",
           policy->capabilities, policy->memory_pages_max,
           policy->cpu_quota_us);

    /* --- Initialise FUSE --- */
    fuse_hal_t hal;
    memset(&hal, 0, sizeof(hal));
    hal.timer_get_timestamp = hal_timer;
    hal.camera_last_frame   = hal_camera;
    hal.quota_arm           = hal_quota_arm;
    hal.quota_cancel        = hal_quota_cancel;

    fuse_stat_t rc = fuse_init(g_module_mem, sizeof(g_module_mem),
                               g_log_mem,    sizeof(g_log_mem),
                               &hal);
    if (rc != FUSE_SUCCESS) {
        fprintf(stderr, "fuse_init failed: %s\n", fuse_stat_name(rc));
        free(aot_buf); free(policy_buf);
        return 1;
    }

    /* --- Load and start module --- */
    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    rc = fuse_module_load(aot_buf, aot_size, policy, &mid);
    if (rc != FUSE_SUCCESS) {
        fprintf(stderr, "fuse_module_load failed: %s\n", fuse_stat_name(rc));
        fuse_stop();
        free(aot_buf); free(policy_buf);
        return 1;
    }

    rc = fuse_module_start(mid);
    if (rc != FUSE_SUCCESS) {
        fprintf(stderr, "fuse_module_start failed: %s\n", fuse_stat_name(rc));
        fuse_module_unload(mid);
        fuse_stop();
        free(aot_buf); free(policy_buf);
        return 1;
    }

    printf("  module id: %u  — running %d steps every %d s\n\n",
           mid, NUM_STEPS, STEP_PERIOD);

    /* --- Periodic step loop --- */
    int ok_count    = 0;
    int quota_count = 0;
    int trap_count  = 0;

    for (int i = 0; i < NUM_STEPS; ++i) {
        printf("[step %d/%d] waiting %d s ...\n", i + 1, NUM_STEPS,
               STEP_PERIOD);
        fflush(stdout);
        sleep(STEP_PERIOD);

        rc = fuse_module_run_step(mid);
        printf("[step %d/%d] result: %s\n", i + 1, NUM_STEPS,
               fuse_stat_name(rc));
        fflush(stdout);

        switch (rc) {
            case FUSE_SUCCESS:            ++ok_count;    break;
            case FUSE_ERR_QUOTA_EXCEEDED: ++quota_count; break;
            default:                       ++trap_count;  break;
        }

        if (rc != FUSE_SUCCESS) {
            printf("  module is no longer runnable — stopping early.\n");
            break;
        }
    }

    /* --- Teardown --- */
    fuse_module_unload(mid);
    fuse_stop();

    printf("\n=== Summary: ok=%d quota_exceeded=%d trap/other=%d ===\n",
           ok_count, quota_count, trap_count);

    free(aot_buf);
    free(policy_buf);
    return 0;
}
