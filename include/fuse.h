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
 * HAL group headers
 *
 * Each hardware group is conditionally compiled when its FUSE_HAL_ENABLE_*
 * flag is defined.  The group header contributes one sub-struct member to
 * fuse_hal_t.
 *
 * IMPORTANT: All FUSE_HAL_ENABLE_* flags must be consistent between the fuse
 * library compile and any consumer (host app, tests) that declares a fuse_hal_t
 * — CMake propagates them via PUBLIC target_compile_definitions.
 *
 * The log group is always present and does NOT appear in fuse_hal_t (it writes
 * to FUSE's internal ring buffer, not a host hardware callback).
 * --------------------------------------------------------------------------- */
#ifdef FUSE_HAL_ENABLE_TEMP_SENSOR
#include "../core/temp/fuse_hal_temp.h"
#endif

#ifdef FUSE_HAL_ENABLE_TIMER
#include "../core/timer/fuse_hal_timer.h"
#endif

#ifdef FUSE_HAL_ENABLE_CAMERA
#include "../core/camera/fuse_hal_camera.h"
#endif

/* ---------------------------------------------------------------------------
 * HAL quota callback typedefs
 *
 * Quota is cross-cutting infrastructure, not a hardware sensor group.
 * Always present regardless of which HAL groups are enabled.
 * --------------------------------------------------------------------------- */

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

/**
 * HAL struct supplied by the Host at fuse_init() time.
 *
 * Only members for enabled hardware groups are compiled in.  All function
 * pointer fields within each group may be NULL; FUSE silently returns 0/void
 * when a NULL callback is invoked.
 *
 * All FUSE_HAL_ENABLE_* compile flags must be identical between the fuse
 * library and any translation unit that declares a fuse_hal_t variable so
 * that the struct layout is consistent.
 */
typedef struct {
#ifdef FUSE_HAL_ENABLE_TEMP_SENSOR
    fuse_hal_temp_group_t    temp;    /**< Temperature sensor group. */
#endif
#ifdef FUSE_HAL_ENABLE_TIMER
    fuse_hal_timer_group_t   timer;   /**< Timer / timestamp group. */
#endif
#ifdef FUSE_HAL_ENABLE_CAMERA
    fuse_hal_camera_group_t  camera;  /**< Camera frame capture group. */
#endif
    fuse_hal_quota_arm_fn    quota_arm;    /**< Arm one-shot quota timer; may be NULL. */
    fuse_hal_quota_cancel_fn quota_cancel; /**< Cancel quota timer; may be NULL. */
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
 * Returns FUSE_ERR_INTERVAL_NOT_ELAPSED if policy.step_interval_us > 0 and
 * the minimum interval since the last successful step has not yet elapsed.
 *
 * The module must be in RUNNING state.
 *
 * @return FUSE_SUCCESS, FUSE_ERR_INVALID_ARG, FUSE_ERR_NOT_INITIALIZED,
 *         FUSE_ERR_QUOTA_EXCEEDED, FUSE_ERR_MODULE_TRAP, or
 *         FUSE_ERR_INTERVAL_NOT_ELAPSED.
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

/**
 * fuse_tick — Run all RUNNING modules whose step interval has elapsed.
 *
 * Iterate every RUNNING module slot. For each module, call
 * fuse_module_run_step(). Modules that return FUSE_ERR_INTERVAL_NOT_ELAPSED
 * are silently skipped (not yet due). Modules that trap or exceed quota have
 * their state updated as usual and are also skipped for this tick.
 *
 * Returns a bitmask of module IDs that completed a step with FUSE_SUCCESS
 * this tick (bit N = module with id N ran). Returns 0 if not initialized,
 * runtime is stopped, or no modules were due.
 *
 * Typical host usage (bare-metal superloop or single RTOS task):
 *   while (1) { usleep(1000); fuse_tick(); }
 */
uint32_t fuse_tick(void);

/**
 * fuse_policy_from_bin — Deserialise a little-endian 32-byte policy binary
 * into a fuse_policy_t.
 *
 * The wire format is 8 x uint32_t little-endian, as produced by
 * tools/policy_to_bin.py and tools/gen_app_config.py.  Use this function
 * when a module's policy arrives at runtime (e.g. uploaded over a link)
 * rather than being compiled in via fuse_app_config.h macros.
 *
 * @param buf        Input buffer — must be at least sizeof(fuse_policy_t) bytes.
 * @param len        Buffer length; must equal sizeof(fuse_policy_t) (32).
 * @param out_policy Output policy struct (written on success only).
 * @returns FUSE_SUCCESS, FUSE_ERR_INVALID_ARG
 */
fuse_stat_t fuse_policy_from_bin(const uint8_t *buf, uint32_t len,
                                  fuse_policy_t *out_policy);

/**
 * fuse_post_event — Signal an event to all subscribed modules.
 *
 * Sets the event bit in the event latch of every RUNNING module whose
 * policy.event_subscribe mask includes event_id.  On the next fuse_tick()
 * call any such module with FUSE_ACTIVATION_EVENT will execute one step.
 *
 * ISR-safe: uses only atomic fetch-or operations.
 *
 * @param event_id  Event identifier, 0-31.
 * @return FUSE_SUCCESS, FUSE_ERR_NOT_INITIALIZED (runtime not initialised),
 *         FUSE_ERR_INVALID_ARG (event_id >= 32).
 */
fuse_stat_t fuse_post_event(uint32_t event_id);

/**
 * fuse_clear_event — Clear all module event latches for event_id.
 *
 * Removes the event bit from every module's event latch, preventing a
 * pending event from triggering further steps.  Useful for draining stale
 * events during shutdown or reset.
 *
 * @param event_id  Event identifier, 0-31.
 * @return FUSE_SUCCESS, FUSE_ERR_NOT_INITIALIZED (runtime not initialised),
 *         FUSE_ERR_INVALID_ARG (event_id >= 32).
 */
fuse_stat_t fuse_clear_event(uint32_t event_id);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_H */
