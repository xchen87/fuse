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

/* Defined in fuse_core.c; used by fuse_hal.c and fuse_module.c. */
extern fuse_context_t g_ctx;

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
 * Internal policy helper (defined in fuse_policy.c)
 * --------------------------------------------------------------------------- */

/**
 * Return true if cap_bit is set in policy->capabilities.
 * Does NOT log or trap — callers must handle violations.
 */
bool fuse_policy_check_cap(const fuse_policy_t *policy, uint32_t cap_bit);

/* ---------------------------------------------------------------------------
 * Internal helper: find descriptor by module instance pointer.
 * Defined in fuse_module.c; used by fuse_hal.c.
 * --------------------------------------------------------------------------- */
fuse_module_desc_t *fuse_module_find_by_inst(wasm_module_inst_t inst);

/* ---------------------------------------------------------------------------
 * Native-function bridge prototypes (defined in fuse_hal.c).
 * These are referenced by the NativeSymbol table in fuse_core.c.
 * They are NOT part of the public API; visibility is limited to core translation units.
 * --------------------------------------------------------------------------- */
float    fuse_native_temp_get_reading(wasm_exec_env_t exec_env);
uint64_t fuse_native_timer_get_timestamp(wasm_exec_env_t exec_env);
uint64_t fuse_native_camera_last_frame(wasm_exec_env_t exec_env,
                                       void *buf, uint32_t max_len);
void     fuse_native_module_log_event(wasm_exec_env_t exec_env,
                                      const char *ptr, uint32_t len,
                                      uint32_t level);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_INTERNAL_H */
