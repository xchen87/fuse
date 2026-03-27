/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse.h — Public FUSE runtime API.
 *
 * The Host includes this header to initialise the runtime, load modules,
 * and drive execution steps.  Modules interact with FUSE exclusively through
 * the native-function bridge defined in core/fuse_hal.c; they never include
 * this header.
 */

#ifndef FUSE_H
#define FUSE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "fuse_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * HAL callback typedefs
 *
 * The Host implements each callback and provides them via fuse_hal_t during
 * fuse_init().  FUSE never calls Host hardware directly — it always routes
 * through these pointers so the same library works on bare-metal, RTOS, and
 * unit-test environments.
 * --------------------------------------------------------------------------- */

/** Read temperature from Host sensor.  Returns Celsius as a float. */
typedef float (*fuse_hal_temp_fn)(void);

/** Return elapsed time in microseconds (monotonic). */
typedef uint64_t (*fuse_hal_timer_fn)(void);

/**
 * Copy the most recent camera frame into buf[0..max_len-1].
 * Returns the actual number of bytes written.
 */
typedef uint64_t (*fuse_hal_camera_fn)(void *buf, uint32_t max_len);

/**
 * Arm a one-shot timer for the given module.
 * When the timer fires, the Host MUST call fuse_quota_expired(module_id).
 * May be NULL; if NULL, quota enforcement is disabled.
 */
typedef void (*fuse_hal_quota_arm_fn)(fuse_module_id_t module_id,
                                     uint32_t quota_us);

/**
 * Cancel the previously armed quota timer for module_id (called after the
 * step returns normally).  May be NULL.
 */
typedef void (*fuse_hal_quota_cancel_fn)(fuse_module_id_t module_id);

/** Collection of all HAL callbacks supplied by the Host at init time. */
typedef struct {
    fuse_hal_temp_fn         temp_get_reading;   /* may be NULL */
    fuse_hal_timer_fn        timer_get_timestamp; /* may be NULL */
    fuse_hal_camera_fn       camera_last_frame;  /* may be NULL */
    fuse_hal_quota_arm_fn    quota_arm;          /* may be NULL */
    fuse_hal_quota_cancel_fn quota_cancel;       /* may be NULL */
} fuse_hal_t;

/* ---------------------------------------------------------------------------
 * FUSE runtime management
 * --------------------------------------------------------------------------- */

/**
 * fuse_init — Initialise the FUSE runtime.
 *
 * Must be called exactly once before any other FUSE API.
 *
 * @param module_memory      Static buffer FUSE uses for WAMR's memory pool.
 * @param module_memory_size Size of module_memory in bytes.
 * @param log_memory         Static buffer for the security-log ring buffer.
 * @param log_memory_size    Size of log_memory in bytes.
 * @param hal                HAL callback table (copied by value).
 *
 * @return FUSE_SUCCESS, FUSE_ERR_INVALID_ARG, or FUSE_ERR_ALREADY_INITIALIZED.
 */
fuse_stat_t fuse_init(void *module_memory, size_t module_memory_size,
                      void *log_memory, size_t log_memory_size,
                      const fuse_hal_t *hal);

/**
 * fuse_stop — Pause the runtime.
 *
 * All RUNNING modules are transitioned to PAUSED.  No new steps may be
 * executed until fuse_restart() is called.
 *
 * @return FUSE_SUCCESS or FUSE_ERR_NOT_INITIALIZED.
 */
fuse_stat_t fuse_stop(void);

/**
 * fuse_restart — Resume the runtime after fuse_stop().
 *
 * @return FUSE_SUCCESS or FUSE_ERR_NOT_INITIALIZED.
 */
fuse_stat_t fuse_restart(void);

/* ---------------------------------------------------------------------------
 * Module management
 * --------------------------------------------------------------------------- */

/**
 * fuse_module_load — Load a WASM/AOT binary and associate a policy with it.
 *
 * The binary buffer must remain valid for the lifetime of the module.  FUSE
 * does not copy the binary; WAMR operates on it in place.
 *
 * @param module_buf  Pointer to the AOT binary (must be 4-byte aligned).
 * @param module_size Size of the binary in bytes.
 * @param policy      Security / resource policy for this module.
 * @param out_id      Receives the assigned module ID on success.
 *
 * @return FUSE_SUCCESS, FUSE_ERR_INVALID_ARG, FUSE_ERR_NOT_INITIALIZED,
 *         FUSE_ERR_MODULE_LIMIT, or FUSE_ERR_MODULE_LOAD_FAILED.
 */
fuse_stat_t fuse_module_load(const uint8_t *module_buf, uint32_t module_size,
                             const fuse_policy_t *policy,
                             fuse_module_id_t *out_id);

/**
 * fuse_module_start — Start or resume a module.
 *
 * Valid from states LOADED or PAUSED.  If the module exports "module_init"
 * and it has not yet been called, it is called here before the state changes
 * to RUNNING.
 *
 * @return FUSE_SUCCESS, FUSE_ERR_INVALID_ARG, FUSE_ERR_NOT_INITIALIZED, or
 *         FUSE_ERR_MODULE_NOT_FOUND.
 */
fuse_stat_t fuse_module_start(fuse_module_id_t id);

/**
 * fuse_module_pause — Request that a module be paused.
 *
 * Valid only from RUNNING state.
 *
 * @return FUSE_SUCCESS, FUSE_ERR_INVALID_ARG, or FUSE_ERR_MODULE_NOT_FOUND.
 */
fuse_stat_t fuse_module_pause(fuse_module_id_t id);

/**
 * fuse_module_stat — Query the current state of a module.
 *
 * @param out_state Receives the current fuse_module_state_t value.
 *
 * @return FUSE_SUCCESS, FUSE_ERR_INVALID_ARG, or FUSE_ERR_MODULE_NOT_FOUND.
 */
fuse_stat_t fuse_module_stat(fuse_module_id_t id,
                             fuse_module_state_t *out_state);

/**
 * fuse_module_unload — Destroy a module and reclaim all its resources.
 *
 * If the module exported "module_deinit" and init was called, deinit is
 * called first.  The slot is returned to the free pool.
 *
 * @return FUSE_SUCCESS, FUSE_ERR_INVALID_ARG, or FUSE_ERR_MODULE_NOT_FOUND.
 */
fuse_stat_t fuse_module_unload(fuse_module_id_t id);

/**
 * fuse_module_run_step — Execute one call to the module's "module_step()" export.
 *
 * Before the call the quota timer is armed (if hal.quota_arm != NULL and
 * policy.cpu_quota_us > 0).  After normal return the timer is cancelled.
 * If the quota fires before the step returns, fuse_quota_expired() terminates
 * the module instance and this function returns FUSE_ERR_QUOTA_EXCEEDED.
 *
 * The module must be in RUNNING state.
 *
 * @return FUSE_SUCCESS, FUSE_ERR_INVALID_ARG, FUSE_ERR_NOT_INITIALIZED,
 *         FUSE_ERR_QUOTA_EXCEEDED, or FUSE_ERR_MODULE_TRAP.
 */
fuse_stat_t fuse_module_run_step(fuse_module_id_t id);

/**
 * fuse_quota_expired — Terminate a module whose CPU quota has been exceeded.
 *
 * Called by the Host from the quota timer ISR (or equivalent).  Safe to call
 * from interrupt context.  Sets the module state to QUOTA_EXCEEDED and
 * terminates the WASM instance so the in-progress wasm_runtime_call_wasm()
 * returns false.
 */
void fuse_quota_expired(fuse_module_id_t module_id);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_H */
