/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_types.h — Shared type definitions for the FUSE runtime.
 *
 * All status codes, module states, capability bits, and the policy
 * structure are defined here and used by both the public API and the
 * internal implementation.
 */

#ifndef FUSE_TYPES_H
#define FUSE_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Status codes
 *
 * FUSE_SUCCESS MUST be 0.  Every public API returns fuse_stat_t.
 * --------------------------------------------------------------------------- */
typedef enum {
    FUSE_SUCCESS               = 0,
    FUSE_ERR_INVALID_ARG       = 1,
    FUSE_ERR_NOT_INITIALIZED   = 2,
    FUSE_ERR_ALREADY_INITIALIZED = 3,
    FUSE_ERR_MODULE_LOAD_FAILED = 4,
    FUSE_ERR_MODULE_NOT_FOUND  = 5,
    FUSE_ERR_MODULE_LIMIT      = 6,
    FUSE_ERR_POLICY_VIOLATION  = 7,
    FUSE_ERR_BUFFER_TOO_SMALL  = 8,
    FUSE_ERR_QUOTA_EXCEEDED         = 9,
    FUSE_ERR_MODULE_TRAP            = 10,
    FUSE_ERR_INTERVAL_NOT_ELAPSED   = 11
} fuse_stat_t;

/* ---------------------------------------------------------------------------
 * Module lifecycle states
 * --------------------------------------------------------------------------- */
typedef enum {
    FUSE_MODULE_STATE_LOADED         = 0,
    FUSE_MODULE_STATE_RUNNING        = 1,
    FUSE_MODULE_STATE_PAUSED         = 2,
    FUSE_MODULE_STATE_TRAPPED        = 3,
    FUSE_MODULE_STATE_QUOTA_EXCEEDED = 4,
    FUSE_MODULE_STATE_UNLOADED       = 5
} fuse_module_state_t;

/* ---------------------------------------------------------------------------
 * Capability bitmask bits — checked against policy before every HAL call
 * --------------------------------------------------------------------------- */
#define FUSE_CAP_TEMP_SENSOR  (1u << 0u)
#define FUSE_CAP_TIMER        (1u << 1u)
#define FUSE_CAP_CAMERA       (1u << 2u)
#define FUSE_CAP_LOG          (1u << 3u)
#define FUSE_CAP_EVENT_POST   (1u << 4u)

/* ---------------------------------------------------------------------------
 * Activation mode bitmask — controls how fuse_tick() triggers a module.
 *
 * FUSE_ACTIVATION_INTERVAL : fuse_tick() triggers based on step_interval_us
 *                            (same as the pre-activation legacy behaviour).
 * FUSE_ACTIVATION_EVENT    : fuse_tick() triggers when subscribed events are
 *                            pending in the module's event latch.
 * FUSE_ACTIVATION_MANUAL   : fuse_tick() skips this module entirely; the host
 *                            drives it exclusively via fuse_module_run_step().
 *
 * Flags may be OR-combined.  activation_mask == 0 is treated as
 * FUSE_ACTIVATION_INTERVAL for backward compatibility.
 * --------------------------------------------------------------------------- */
#define FUSE_ACTIVATION_INTERVAL  (1u << 0u)
#define FUSE_ACTIVATION_EVENT     (1u << 1u)
#define FUSE_ACTIVATION_MANUAL    (1u << 2u)

/* ---------------------------------------------------------------------------
 * Module policy
 *
 * capabilities     : bitmask of FUSE_CAP_* bits granted to this module
 * memory_pages_max : maximum WASM linear-memory pages (64 KiB each)
 * stack_size       : execution-stack size in bytes
 * heap_size        : module heap size in bytes
 * cpu_quota_us     : maximum single-step wall-clock time in microseconds
 *                    (0 = unlimited)
 * step_interval_us : minimum microseconds between consecutive steps
 *                    (0 = no constraint)
 * activation_mask  : FUSE_ACTIVATION_* bitmask; 0 treated as INTERVAL (compat)
 * event_subscribe  : bitmask of event IDs (bits 0–31) that trigger this module
 *                    when FUSE_ACTIVATION_EVENT is set in activation_mask
 * --------------------------------------------------------------------------- */
typedef struct {
    uint32_t capabilities;
    uint32_t memory_pages_max;
    uint32_t stack_size;
    uint32_t heap_size;
    uint32_t cpu_quota_us;
    uint32_t step_interval_us;  /* min µs between steps; 0 = no constraint */
    uint32_t activation_mask;   /* FUSE_ACTIVATION_* bitmask; 0 = INTERVAL (compat) */
    uint32_t event_subscribe;   /* bitmask of event IDs (0-31) that trigger this module */
} fuse_policy_t;  /* 32 bytes, 8 x uint32_t little-endian */

/* ---------------------------------------------------------------------------
 * Module identifier
 * --------------------------------------------------------------------------- */
typedef uint32_t fuse_module_id_t;

#define FUSE_INVALID_MODULE_ID  ((fuse_module_id_t)UINT32_MAX)
#define FUSE_MAX_MODULES        (8u)

/* Maximum byte length of a single security-log message (including NUL). */
#define FUSE_LOG_MSG_MAX        (128u)

#ifdef __cplusplus
}
#endif

#endif /* FUSE_TYPES_H */
