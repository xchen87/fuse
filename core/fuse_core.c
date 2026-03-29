/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_core.c — FUSE runtime initialisation and lifecycle management.
 *
 * This translation unit owns the single global fuse_context_t (g_ctx) and
 * implements fuse_init(), fuse_stop(), fuse_restart(), and the quota-expiry
 * interrupt handler fuse_quota_expired().
 *
 * HAL native symbols are registered per hardware group via
 * fuse_hal_<group>_register_natives() calls in fuse_init(), after
 * wasm_runtime_full_init() succeeds.  Each group source file owns its own
 * NativeSymbol table and registration function.
 */

#include "fuse_internal.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Global runtime context (definition — extern declared in fuse_internal.h)
 * --------------------------------------------------------------------------- */
fuse_context_t g_ctx;

/* ---------------------------------------------------------------------------
 * fuse_init
 * --------------------------------------------------------------------------- */
fuse_stat_t fuse_init(void *module_memory, size_t module_memory_size,
                      void *log_memory, size_t log_memory_size,
                      const fuse_hal_t *hal)
{
    RuntimeInitArgs  init_args;
    uint32_t         log_capacity;

    /* -- Input validation -------------------------------------------------- */
    if (module_memory == NULL) {
        return FUSE_ERR_INVALID_ARG;
    }
    if (module_memory_size == 0u) {
        return FUSE_ERR_INVALID_ARG;
    }
    if (log_memory == NULL) {
        return FUSE_ERR_INVALID_ARG;
    }
    if (log_memory_size == 0u) {
        return FUSE_ERR_INVALID_ARG;
    }
    if (hal == NULL) {
        return FUSE_ERR_INVALID_ARG;
    }

    /* -- Guard against double-init ----------------------------------------- */
    if (g_ctx.initialized) {
        return FUSE_ERR_ALREADY_INITIALIZED;
    }

    /* -- Zero the entire global context ------------------------------------ */
    (void)memset(&g_ctx, 0, sizeof(g_ctx));

    /* -- Copy HAL callbacks ------------------------------------------------ */
    (void)memcpy(&g_ctx.hal, hal, sizeof(fuse_hal_t));

    /* -- Configure WAMR init args ----------------------------------------- */
    (void)memset(&init_args, 0, sizeof(init_args));

    init_args.mem_alloc_type              = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf  = module_memory;
    init_args.mem_alloc_option.pool.heap_size =
        (uint32_t)((module_memory_size > (size_t)UINT32_MAX)
                   ? UINT32_MAX
                   : (uint32_t)module_memory_size);

    /* -- Initialise WAMR --------------------------------------------------- */
    if (!wasm_runtime_full_init(&init_args)) {
        return FUSE_ERR_MODULE_LOAD_FAILED;
    }

    /* -- Register HAL native symbols per enabled hardware group ------------ */
#ifdef FUSE_HAL_ENABLE_TEMP_SENSOR
    fuse_hal_temp_register_natives();
#endif
#ifdef FUSE_HAL_ENABLE_TIMER
    fuse_hal_timer_register_natives();
#endif
#ifdef FUSE_HAL_ENABLE_CAMERA
    fuse_hal_camera_register_natives();
#endif
    fuse_hal_log_register_natives();    /* log group is always registered */
    fuse_hal_event_register_natives();  /* event group is always registered */

    /* -- Initialise security-log ring buffer ------------------------------- */
    log_capacity = (uint32_t)(log_memory_size / sizeof(fuse_log_entry_t));
    if (log_capacity == 0u) {
        /* log_memory_size is too small to hold even one entry; still allow
         * init but logging will silently drop messages (checked in
         * fuse_log_write). */
    }

    g_ctx.log_ctx.entries   = (fuse_log_entry_t *)log_memory;
    g_ctx.log_ctx.capacity  = log_capacity;
    g_ctx.log_ctx.write_idx = 0u;

    /* -- Store memory pointers --------------------------------------------- */
    g_ctx.module_memory      = (uint8_t *)module_memory;
    g_ctx.module_memory_size = module_memory_size;

    /* -- Mark runtime as operational --------------------------------------- */
    g_ctx.initialized = true;
    g_ctx.running     = true;

    fuse_log_write(&g_ctx.log_ctx, FUSE_INVALID_MODULE_ID, 1u,
                   "FUSE runtime initialised");

    return FUSE_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * fuse_stop
 * --------------------------------------------------------------------------- */
fuse_stat_t fuse_stop(void)
{
    uint32_t i;

    if (!g_ctx.initialized) {
        return FUSE_ERR_NOT_INITIALIZED;
    }

    g_ctx.running = false;

    /* Pause every RUNNING module so fuse_module_run_step() rejects calls. */
    for (i = 0u; i < FUSE_MAX_MODULES; i++) {
        if (g_ctx.modules[i].in_use &&
            (g_ctx.modules[i].state == FUSE_MODULE_STATE_RUNNING)) {
            g_ctx.modules[i].state = FUSE_MODULE_STATE_PAUSED;
        }
    }

    fuse_log_write(&g_ctx.log_ctx, FUSE_INVALID_MODULE_ID, 1u,
                   "FUSE runtime stopped");

    return FUSE_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * fuse_restart
 * --------------------------------------------------------------------------- */
fuse_stat_t fuse_restart(void)
{
    if (!g_ctx.initialized) {
        return FUSE_ERR_NOT_INITIALIZED;
    }

    g_ctx.running = true;

    fuse_log_write(&g_ctx.log_ctx, FUSE_INVALID_MODULE_ID, 1u,
                   "FUSE runtime restarted");

    return FUSE_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * fuse_quota_expired
 *
 * Called from timer ISR.  Must be signal/interrupt safe.  Only calls
 * wasm_runtime_terminate() which is documented as async-signal-safe.
 * --------------------------------------------------------------------------- */
void fuse_quota_expired(fuse_module_id_t module_id)
{
    fuse_module_desc_t *desc;
    uint32_t            i;

    if (!g_ctx.initialized) {
        return;
    }

    if (module_id >= FUSE_MAX_MODULES) {
        return;
    }

    desc = NULL;
    for (i = 0u; i < FUSE_MAX_MODULES; i++) {
        if (g_ctx.modules[i].in_use && (g_ctx.modules[i].id == module_id)) {
            desc = &g_ctx.modules[i];
            break;
        }
    }

    if (desc == NULL) {
        return;
    }

    if (desc->module_inst == NULL) {
        return;
    }

    /* Mark state before terminating so fuse_module_run_step() sees it.
     * The SEQ_CST fence ensures the state write is globally visible on
     * weakly-ordered targets (e.g. ARM) before the terminate call. */
    desc->state = FUSE_MODULE_STATE_QUOTA_EXCEEDED;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    /* Interrupt the in-progress wasm_runtime_call_wasm(). */
    wasm_runtime_terminate(desc->module_inst);
}
