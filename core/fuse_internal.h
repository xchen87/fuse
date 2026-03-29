/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_internal.h — Private types and declarations shared among FUSE core
 *                   translation units only.
 *
 * This header MUST NOT be included by external callers or test code.
 */

#ifndef FUSE_INTERNAL_H
#define FUSE_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* C11 <stdatomic.h> uses _Atomic which is not valid in C++ mode under GCC.
 * When compiled as C++, pull in <atomic> and map the C11 function-like macros
 * to their std::atomic equivalents so that fuse_module_desc_t remains usable
 * from C++ test translation units that include this header via fuse_test_helper.h. */
#ifdef __cplusplus
   /* C++ callers (test translation units) use <atomic> from the standard library.
    * Provide inline wrappers matching the C11 atomic_*_explicit function signatures
    * so test code that reads event_latch can use the same macro names as C code. */
#  include <atomic>
#  ifndef atomic_load_explicit
#    define atomic_load_explicit(obj, order) ((obj)->load(order))
#  endif
#  ifndef atomic_fetch_or_explicit
#    define atomic_fetch_or_explicit(obj, val, order) ((obj)->fetch_or((val), (order)))
#  endif
#  ifndef atomic_fetch_and_explicit
#    define atomic_fetch_and_explicit(obj, val, order) ((obj)->fetch_and((val), (order)))
#  endif
#  ifndef memory_order_relaxed
#    define memory_order_relaxed std::memory_order_relaxed
#  endif
#  ifndef memory_order_acquire
#    define memory_order_acquire std::memory_order_acquire
#  endif
#  ifndef memory_order_acq_rel
#    define memory_order_acq_rel std::memory_order_acq_rel
#  endif
#else
#  include <stdatomic.h>
#endif

#include "fuse_types.h"
#include "fuse.h"
#include "wasm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Security-log ring-buffer context
 * --------------------------------------------------------------------------- */

/** One entry in the security log. */
typedef struct {
    uint64_t          timestamp_us;
    fuse_module_id_t  module_id;
    uint32_t          level;          /* 0=DEBUG, 1=INFO, 2=FATAL */
    char              message[FUSE_LOG_MSG_MAX];
} fuse_log_entry_t;

/** Ring-buffer descriptor for the security log. */
typedef struct {
    fuse_log_entry_t *entries;   /* pointer into host-provided log_memory  */
    uint32_t          capacity;  /* total number of entry slots available  */
    uint32_t          write_idx; /* next slot to write (modulo capacity)   */
} fuse_log_ctx_t;

/* ---------------------------------------------------------------------------
 * Per-module descriptor (internal)
 * --------------------------------------------------------------------------- */

typedef struct {
    bool                  in_use;
    fuse_module_state_t   state;
    fuse_policy_t         policy;
    fuse_module_id_t      id;

    wasm_module_t         wasm_module;
    wasm_module_inst_t    module_inst;
    wasm_exec_env_t       exec_env;

    wasm_function_inst_t  fn_step;    /* required export "module_step"   */
    wasm_function_inst_t  fn_init;    /* optional export "module_init"   */
    wasm_function_inst_t  fn_deinit;  /* optional export "module_deinit" */

    bool                  init_called;
    bool                  step_ever_run;   /* true after first successful step; avoids 0-sentinel collision */
    uint64_t              last_step_at_us; /* step_start_us recorded at last successful step */

    /* Event latch: bits set by fuse_post_event() for events this module
     * subscribes to.  Cleared atomically after the triggered step runs.
     * ISR-safe via C11 _Atomic (C) / std::atomic<uint32_t> (C++). */
#ifdef __cplusplus
    std::atomic<uint32_t> event_latch;
#else
    _Atomic uint32_t      event_latch;
#endif
} fuse_module_desc_t;

/* ---------------------------------------------------------------------------
 * Global FUSE context
 * --------------------------------------------------------------------------- */

typedef struct {
    bool                initialized;
    bool                running;
    fuse_module_desc_t  modules[FUSE_MAX_MODULES];
    uint8_t            *module_memory;
    size_t              module_memory_size;
    fuse_log_ctx_t      log_ctx;
    fuse_hal_t          hal;
} fuse_context_t;

/* Defined in fuse_core.c; used by HAL group files and fuse_module.c. */
extern fuse_context_t g_ctx;

/* ---------------------------------------------------------------------------
 * Log HAL group — always compiled.
 * Included here so fuse_core.c can call fuse_hal_log_register_natives().
 * --------------------------------------------------------------------------- */
#include "log/fuse_hal_log.h"

/* ---------------------------------------------------------------------------
 * Event HAL group — always compiled.
 * Included here so fuse_core.c can call fuse_hal_event_register_natives().
 * --------------------------------------------------------------------------- */
#include "event/fuse_hal_event.h"

/* ---------------------------------------------------------------------------
 * Internal log helper (defined in fuse_log.c)
 * --------------------------------------------------------------------------- */

/**
 * Write one message to the security-log ring buffer.
 *
 * Thread-safety: single-threaded by FUSE design; no locking required.
 * ISR-safety   : fuse_quota_expired() only calls wasm_runtime_terminate(),
 *                not fuse_log_write(), so no ISR re-entrancy concern.
 */
void fuse_log_write(fuse_log_ctx_t *ctx, fuse_module_id_t id,
                    uint32_t level, const char *msg);

/* ---------------------------------------------------------------------------
 * Internal policy helpers (defined in fuse_policy.c)
 * --------------------------------------------------------------------------- */

/**
 * Return true if cap_bit is set in policy->capabilities.
 * Does NOT log or trap — callers must handle violations.
 */
bool fuse_policy_check_cap(const fuse_policy_t *policy, uint32_t cap_bit);

/**
 * Log a FATAL security event, set a WASM exception, and transition the
 * module to TRAPPED state.  Shared by all HAL group bridge implementations.
 */
void fuse_policy_violation(fuse_module_desc_t *desc,
                           wasm_module_inst_t inst,
                           const char *cap_name);

/* ---------------------------------------------------------------------------
 * Internal helper: find descriptor by module instance pointer.
 * Defined in fuse_module.c; used by HAL group bridge files.
 * --------------------------------------------------------------------------- */
fuse_module_desc_t *fuse_module_find_by_inst(wasm_module_inst_t inst);

/* ---------------------------------------------------------------------------
 * fuse_hal_resolve_desc — shared entry guard for all HAL native bridges.
 *
 * Resolves exec_env to a validated module descriptor.  *inst_out is always
 * set before returning so callers can forward it to fuse_policy_violation()
 * or wasm_runtime_validate_native_addr() regardless of the return value.
 *
 * Returns: non-NULL descriptor on success.
 *          NULL when the instance is unknown (rogue exec_env) — the
 *          "rogue exec_env" exception is set on the instance in that case,
 *          or the instance itself is NULL (exec_env is being destroyed).
 * --------------------------------------------------------------------------- */
static inline fuse_module_desc_t *
fuse_hal_resolve_desc(wasm_exec_env_t     exec_env,
                      wasm_module_inst_t *inst_out)
{
    wasm_module_inst_t  inst;
    fuse_module_desc_t *desc;

    inst      = wasm_runtime_get_module_inst(exec_env);
    *inst_out = inst;

    if (inst == NULL) {
        return NULL;
    }

    desc = fuse_module_find_by_inst(inst);
    if (desc == NULL) {
        fuse_log_write(&g_ctx.log_ctx, FUSE_INVALID_MODULE_ID, 2u,
                       "rogue exec_env");
        wasm_runtime_set_exception(inst, "FUSE: rogue exec_env");
    }

    return desc;
}

#ifdef __cplusplus
}
#endif

#endif /* FUSE_INTERNAL_H */
