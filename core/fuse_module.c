/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_module.c — Module lifecycle management: load, start, pause, stat,
 *                 unload, and per-step execution with quota enforcement.
 */

#include "fuse_internal.h"

#include <string.h>
#include <stdio.h>

/* Error-buffer size for WAMR API calls. */
#define WAMR_ERR_BUF_SIZE  (128u)

/* ---------------------------------------------------------------------------
 * Internal helper: find a descriptor by module ID.
 * Returns NULL if the ID is not in use.
 * --------------------------------------------------------------------------- */
static fuse_module_desc_t *find_desc_by_id(fuse_module_id_t id)
{
    uint32_t i;

    if (id >= FUSE_MAX_MODULES) {
        return NULL;
    }

    for (i = 0u; i < FUSE_MAX_MODULES; i++) {
        if (g_ctx.modules[i].in_use && (g_ctx.modules[i].id == id)) {
            return &g_ctx.modules[i];
        }
    }

    return NULL;
}

/* ---------------------------------------------------------------------------
 * fuse_module_find_by_inst (used by fuse_hal.c)
 * --------------------------------------------------------------------------- */
fuse_module_desc_t *fuse_module_find_by_inst(wasm_module_inst_t inst)
{
    uint32_t i;

    if (inst == NULL) {
        return NULL;
    }

    for (i = 0u; i < FUSE_MAX_MODULES; i++) {
        if (g_ctx.modules[i].in_use &&
            (g_ctx.modules[i].module_inst == inst)) {
            return &g_ctx.modules[i];
        }
    }

    return NULL;
}

/* ---------------------------------------------------------------------------
 * fuse_module_load
 * --------------------------------------------------------------------------- */
fuse_stat_t fuse_module_load(const uint8_t *module_buf, uint32_t module_size,
                             const fuse_policy_t *policy,
                             fuse_module_id_t *out_id)
{
    char                error_buf[WAMR_ERR_BUF_SIZE];
    fuse_module_desc_t *slot;
    uint32_t            i;
    InstantiationArgs   inst_args;
    char                log_msg[FUSE_LOG_MSG_MAX];

    /* -- Input validation -------------------------------------------------- */
    if (module_buf == NULL) {
        return FUSE_ERR_INVALID_ARG;
    }
    if (module_size == 0u) {
        return FUSE_ERR_INVALID_ARG;
    }
    if (policy == NULL) {
        return FUSE_ERR_INVALID_ARG;
    }
    if (out_id == NULL) {
        return FUSE_ERR_INVALID_ARG;
    }

    if (!g_ctx.initialized) {
        return FUSE_ERR_NOT_INITIALIZED;
    }

    /* -- Find a free slot -------------------------------------------------- */
    slot = NULL;
    for (i = 0u; i < FUSE_MAX_MODULES; i++) {
        if (!g_ctx.modules[i].in_use) {
            slot = &g_ctx.modules[i];
            break;
        }
    }

    if (slot == NULL) {
        return FUSE_ERR_MODULE_LIMIT;
    }

    /* Zero the slot before use. */
    (void)memset(slot, 0, sizeof(*slot));

    /* -- wasm_runtime_load ------------------------------------------------- */
    (void)memset(error_buf, 0, sizeof(error_buf));

    /*
     * WAMR's wasm_runtime_load() takes a non-const uint8_t* because it may
     * patch the binary for AOT relocation.  The caller contract requires the
     * buffer to remain alive and writable.  We cast away const here knowing
     * the caller provided a writable buffer (AOT binary in host RAM).
     */
    slot->wasm_module = wasm_runtime_load(
        (uint8_t *)(uintptr_t)module_buf,
        module_size,
        error_buf,
        (uint32_t)sizeof(error_buf));

    if (slot->wasm_module == NULL) {
        (void)snprintf(log_msg, sizeof(log_msg),
                       "wasm_runtime_load failed: %.80s", error_buf);
        fuse_log_write(&g_ctx.log_ctx, FUSE_INVALID_MODULE_ID, 2u, log_msg);
        return FUSE_ERR_MODULE_LOAD_FAILED;
    }

    /* -- wasm_runtime_instantiate_ex --------------------------------------- */
    (void)memset(&inst_args, 0, sizeof(inst_args));
    inst_args.default_stack_size     = policy->stack_size;
    inst_args.host_managed_heap_size = policy->heap_size;
    inst_args.max_memory_pages       = policy->memory_pages_max;

    (void)memset(error_buf, 0, sizeof(error_buf));
    slot->module_inst = wasm_runtime_instantiate_ex(
        slot->wasm_module,
        &inst_args,
        error_buf,
        (uint32_t)sizeof(error_buf));

    if (slot->module_inst == NULL) {
        (void)snprintf(log_msg, sizeof(log_msg),
                       "instantiate failed: %.80s", error_buf);
        fuse_log_write(&g_ctx.log_ctx, FUSE_INVALID_MODULE_ID, 2u, log_msg);
        wasm_runtime_unload(slot->wasm_module);
        slot->wasm_module = NULL;
        return FUSE_ERR_MODULE_LOAD_FAILED;
    }

    /* -- Create execution environment ------------------------------------- */
    slot->exec_env = wasm_runtime_create_exec_env(
        slot->module_inst, policy->stack_size);

    if (slot->exec_env == NULL) {
        fuse_log_write(&g_ctx.log_ctx, FUSE_INVALID_MODULE_ID, 2u,
                       "create_exec_env failed");
        wasm_runtime_deinstantiate(slot->module_inst);
        wasm_runtime_unload(slot->wasm_module);
        slot->module_inst = NULL;
        slot->wasm_module = NULL;
        return FUSE_ERR_MODULE_LOAD_FAILED;
    }

    /* -- Lookup required export: "module_step" ---------------------------- */
    slot->fn_step = wasm_runtime_lookup_function(slot->module_inst,
                                                 "module_step");
    if (slot->fn_step == NULL) {
        fuse_log_write(&g_ctx.log_ctx, FUSE_INVALID_MODULE_ID, 2u,
                       "module_step export not found");
        wasm_runtime_destroy_exec_env(slot->exec_env);
        wasm_runtime_deinstantiate(slot->module_inst);
        wasm_runtime_unload(slot->wasm_module);
        slot->exec_env    = NULL;
        slot->module_inst = NULL;
        slot->wasm_module = NULL;
        return FUSE_ERR_MODULE_LOAD_FAILED;
    }

    /* -- Optional exports ------------------------------------------------- */
    slot->fn_init   = wasm_runtime_lookup_function(slot->module_inst,
                                                   "module_init");
    slot->fn_deinit = wasm_runtime_lookup_function(slot->module_inst,
                                                   "module_deinit");

    /* -- Populate descriptor ---------------------------------------------- */
    slot->policy      = *policy;
    slot->id          = (fuse_module_id_t)i;
    slot->state       = FUSE_MODULE_STATE_LOADED;
    slot->init_called = false;
    slot->in_use      = true;

    *out_id = slot->id;

    (void)snprintf(log_msg, sizeof(log_msg),
                   "module %u loaded", (unsigned int)slot->id);
    fuse_log_write(&g_ctx.log_ctx, slot->id, 1u, log_msg);

    return FUSE_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * fuse_module_start
 * --------------------------------------------------------------------------- */
fuse_stat_t fuse_module_start(fuse_module_id_t id)
{
    fuse_module_desc_t *desc;
    char                log_msg[FUSE_LOG_MSG_MAX];

    if (!g_ctx.initialized) {
        return FUSE_ERR_NOT_INITIALIZED;
    }

    desc = find_desc_by_id(id);
    if (desc == NULL) {
        return FUSE_ERR_MODULE_NOT_FOUND;
    }

    /* Only LOADED or PAUSED modules can be started/resumed. */
    if ((desc->state != FUSE_MODULE_STATE_LOADED) &&
        (desc->state != FUSE_MODULE_STATE_PAUSED)) {
        return FUSE_ERR_INVALID_ARG;
    }

    /* Call module_init() once if it was not yet called. */
    if ((desc->fn_init != NULL) && !desc->init_called) {
        if (!wasm_runtime_call_wasm(desc->exec_env, desc->fn_init, 0u, NULL)) {
            const char *exc = wasm_runtime_get_exception(desc->module_inst);
            (void)snprintf(log_msg, sizeof(log_msg),
                           "module_init trap mod=%u: %.70s",
                           (unsigned int)id,
                           (exc != NULL) ? exc : "unknown");
            fuse_log_write(&g_ctx.log_ctx, id, 2u, log_msg);
            desc->state = FUSE_MODULE_STATE_TRAPPED;
            return FUSE_ERR_MODULE_TRAP;
        }
        desc->init_called = true;
    }

    desc->state = FUSE_MODULE_STATE_RUNNING;

    (void)snprintf(log_msg, sizeof(log_msg),
                   "module %u running", (unsigned int)id);
    fuse_log_write(&g_ctx.log_ctx, id, 0u, log_msg);

    return FUSE_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * fuse_module_pause
 * --------------------------------------------------------------------------- */
fuse_stat_t fuse_module_pause(fuse_module_id_t id)
{
    fuse_module_desc_t *desc;
    char                log_msg[FUSE_LOG_MSG_MAX];

    if (!g_ctx.initialized) {
        return FUSE_ERR_NOT_INITIALIZED;
    }

    desc = find_desc_by_id(id);
    if (desc == NULL) {
        return FUSE_ERR_MODULE_NOT_FOUND;
    }

    if (desc->state != FUSE_MODULE_STATE_RUNNING) {
        return FUSE_ERR_INVALID_ARG;
    }

    desc->state = FUSE_MODULE_STATE_PAUSED;

    (void)snprintf(log_msg, sizeof(log_msg),
                   "module %u paused", (unsigned int)id);
    fuse_log_write(&g_ctx.log_ctx, id, 0u, log_msg);

    return FUSE_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * fuse_module_stat
 * --------------------------------------------------------------------------- */
fuse_stat_t fuse_module_stat(fuse_module_id_t id,
                             fuse_module_state_t *out_state)
{
    const fuse_module_desc_t *desc;

    if (out_state == NULL) {
        return FUSE_ERR_INVALID_ARG;
    }

    if (!g_ctx.initialized) {
        return FUSE_ERR_NOT_INITIALIZED;
    }

    desc = find_desc_by_id(id);
    if (desc == NULL) {
        return FUSE_ERR_MODULE_NOT_FOUND;
    }

    *out_state = desc->state;

    return FUSE_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * fuse_module_unload
 * --------------------------------------------------------------------------- */
fuse_stat_t fuse_module_unload(fuse_module_id_t id)
{
    fuse_module_desc_t *desc;
    char                log_msg[FUSE_LOG_MSG_MAX];

    if (!g_ctx.initialized) {
        return FUSE_ERR_NOT_INITIALIZED;
    }

    desc = find_desc_by_id(id);
    if (desc == NULL) {
        return FUSE_ERR_MODULE_NOT_FOUND;
    }

    /* Call module_deinit() if applicable.
     * Skip when the WAMR instance has already been terminated (TRAPPED or
     * QUOTA_EXCEEDED): re-entering a terminated instance is undefined.
     * NOTE: g_ctx.running == false (after fuse_stop()) does NOT prevent this
     * call — fuse_stop() only sets FUSE's scheduling flag and transitions
     * modules to PAUSED; WAMR's exec_env remains valid and callable for
     * lifecycle operations like deinit.  The PAUSED state is intentionally
     * included in the set of states for which deinit should run. */
    if ((desc->fn_deinit != NULL) && desc->init_called &&
        (desc->state != FUSE_MODULE_STATE_TRAPPED) &&
        (desc->state != FUSE_MODULE_STATE_QUOTA_EXCEEDED)) {
        /* Best-effort: ignore trap during deinit. */
        (void)wasm_runtime_call_wasm(desc->exec_env, desc->fn_deinit,
                                     0u, NULL);
    }

    /* Tear down WAMR resources in reverse order of creation. */
    if (desc->exec_env != NULL) {
        wasm_runtime_destroy_exec_env(desc->exec_env);
        desc->exec_env = NULL;
    }

    if (desc->module_inst != NULL) {
        wasm_runtime_deinstantiate(desc->module_inst);
        desc->module_inst = NULL;
    }

    if (desc->wasm_module != NULL) {
        wasm_runtime_unload(desc->wasm_module);
        desc->wasm_module = NULL;
    }

    (void)snprintf(log_msg, sizeof(log_msg),
                   "module %u unloaded", (unsigned int)id);
    fuse_log_write(&g_ctx.log_ctx, id, 1u, log_msg);

    /* Zero the whole descriptor (sets in_use to false, all pointers to NULL). */
    (void)memset(desc, 0, sizeof(*desc));

    return FUSE_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * fuse_module_run_step
 * --------------------------------------------------------------------------- */
fuse_stat_t fuse_module_run_step(fuse_module_id_t id)
{
    fuse_module_desc_t *desc;
    bool                call_ok;
    const char         *exc;
    char                log_msg[FUSE_LOG_MSG_MAX];
    uint64_t            step_start_us;

    if (!g_ctx.initialized) {
        return FUSE_ERR_NOT_INITIALIZED;
    }

    if (!g_ctx.running) {
        return FUSE_ERR_INVALID_ARG;
    }

    desc = find_desc_by_id(id);
    if (desc == NULL) {
        return FUSE_ERR_MODULE_NOT_FOUND;
    }

    if (desc->state != FUSE_MODULE_STATE_RUNNING) {
        return FUSE_ERR_INVALID_ARG;
    }

    /* -- Check step interval -------------------------------------------- */
    step_start_us = 0u;
#ifdef FUSE_HAL_ENABLE_TIMER
    if (g_ctx.hal.timer.get_timestamp != NULL) {
        step_start_us = g_ctx.hal.timer.get_timestamp();
    }
#endif
    if ((desc->policy.step_interval_us > 0u) && desc->step_ever_run &&
#ifdef FUSE_HAL_ENABLE_TIMER
        (g_ctx.hal.timer.get_timestamp != NULL)) {
#else
        false) {
#endif
        /* Guard against unsigned underflow when the clock is reset or the
         * HAL is swapped (step_start_us < last_step_at_us).  In that case
         * we skip enforcement rather than wrapping to a huge elapsed value
         * that would spuriously bypass the rate limit. */
        if ((step_start_us >= desc->last_step_at_us) &&
            ((step_start_us - desc->last_step_at_us) <
             (uint64_t)desc->policy.step_interval_us)) {
            return FUSE_ERR_INTERVAL_NOT_ELAPSED;
        }
    }

    /* -- Arm quota timer if configured ------------------------------------ */
    if ((desc->policy.cpu_quota_us > 0u) &&
        (g_ctx.hal.quota_arm != NULL)) {
        g_ctx.hal.quota_arm(id, desc->policy.cpu_quota_us);
    }

    /* -- Execute one step ------------------------------------------------- */
    call_ok = wasm_runtime_call_wasm(desc->exec_env, desc->fn_step, 0u, NULL);

    /* -- Cancel quota timer (normal return path) -------------------------- */
    if ((desc->policy.cpu_quota_us > 0u) &&
        (g_ctx.hal.quota_cancel != NULL)) {
        g_ctx.hal.quota_cancel(id);
    }

    /* Fence ensures the ISR's state write (QUOTA_EXCEEDED) is visible before
     * we read desc->state below, preventing reordering on weakly-ordered
     * architectures where the signal handler and main thread may share a core. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    /* -- Handle failure --------------------------------------------------- */
    if (!call_ok) {
        /* Check if the failure was due to quota expiry (ISR already set
         * state to QUOTA_EXCEEDED and called wasm_runtime_terminate()). */
        if (desc->state == FUSE_MODULE_STATE_QUOTA_EXCEEDED) {
            (void)snprintf(log_msg, sizeof(log_msg),
                           "module %u quota exceeded", (unsigned int)id);
            fuse_log_write(&g_ctx.log_ctx, id, 2u, log_msg);
            return FUSE_ERR_QUOTA_EXCEEDED;
        }

        /* Otherwise it is a WASM trap. */
        exc = wasm_runtime_get_exception(desc->module_inst);
        (void)snprintf(log_msg, sizeof(log_msg),
                       "module %u trap: %.80s",
                       (unsigned int)id,
                       (exc != NULL) ? exc : "unknown");
        fuse_log_write(&g_ctx.log_ctx, id, 2u, log_msg);
        desc->state = FUSE_MODULE_STATE_TRAPPED;
        return FUSE_ERR_MODULE_TRAP;
    }

    /* -- Record successful step start time -------------------------------- */
    desc->last_step_at_us = step_start_us;
    desc->step_ever_run   = true;
    return FUSE_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * fuse_tick
 * --------------------------------------------------------------------------- */
uint32_t fuse_tick(void)
{
    uint32_t            i;
    uint32_t            ran_mask = 0u;
    fuse_module_desc_t *desc;
    fuse_stat_t         rc;

    /* Compile-time guard: the bitmask return type must be wide enough for all
     * module IDs.  Raise this if FUSE_MAX_MODULES is ever increased past 32. */
    _Static_assert(FUSE_MAX_MODULES <= 32u,
                   "fuse_tick bitmask (uint32_t) too narrow for FUSE_MAX_MODULES");

    if (!g_ctx.initialized || !g_ctx.running) {
        return 0u;
    }

    for (i = 0u; i < FUSE_MAX_MODULES; ++i) {
        desc = &g_ctx.modules[i];

        if (!desc->in_use ||
            (desc->state != FUSE_MODULE_STATE_RUNNING)) {
            continue;
        }

        rc = fuse_module_run_step(desc->id);
        if (rc == FUSE_SUCCESS) {
            ran_mask |= ((uint32_t)1u << desc->id);
        }
        /* FUSE_ERR_INTERVAL_NOT_ELAPSED: not yet due — expected, continue */
        /* FUSE_ERR_QUOTA_EXCEEDED / FUSE_ERR_MODULE_TRAP: state already
         * updated; continue to next module */
    }

    return ran_mask;
}
