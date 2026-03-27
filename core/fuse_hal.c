/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_hal.c — Native-function bridge implementations.
 *
 * Each function in this file corresponds to one symbol in the WAMR
 * NativeSymbol table registered in fuse_core.c.  When a WASM module calls
 * e.g. "temp_get_reading", WAMR dispatches into fuse_native_temp_get_reading()
 * here.
 *
 * Security contract (enforced for every HAL function):
 *   1. Retrieve the calling module instance from the exec_env.
 *   2. Look up the module descriptor to inspect its policy.
 *   3. Check the required capability bit with fuse_policy_check_cap().
 *   4. On violation: log FATAL, set exception, set state=TRAPPED, return 0.
 *   5. On success: delegate to the corresponding g_ctx.hal callback.
 *
 * Memory-access safety (camera_last_frame, module_log_event):
 *   WAMR's '*' signature auto-converts the app-offset to a native pointer
 *   before calling us; we validate that native pointer with
 *   wasm_runtime_validate_native_addr() to confirm it is within the module's
 *   linear memory before reading/writing any bytes.
 */

#include "fuse_internal.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Internal helper: handle a policy violation
 *
 * Logs a FATAL security event, sets a WASM exception so the calling
 * wasm_runtime_call_wasm() returns false, and transitions the module to
 * TRAPPED state.
 * --------------------------------------------------------------------------- */
static void policy_violation(fuse_module_desc_t *desc,
                             wasm_module_inst_t inst,
                             const char *cap_name)
{
    char log_msg[FUSE_LOG_MSG_MAX];

    (void)snprintf(log_msg, sizeof(log_msg),
                   "SECURITY: module %u policy violation cap=%s",
                   (desc != NULL) ? (unsigned int)desc->id : 0xFFFFFFFFu,
                   (cap_name != NULL) ? cap_name : "?");

    fuse_log_write(&g_ctx.log_ctx,
                   (desc != NULL) ? desc->id : FUSE_INVALID_MODULE_ID,
                   2u,
                   log_msg);

    wasm_runtime_set_exception(inst, "FUSE policy violation");

    if (desc != NULL) {
        desc->state = FUSE_MODULE_STATE_TRAPPED;
    }
}

/* ---------------------------------------------------------------------------
 * fuse_native_temp_get_reading
 *
 * WASM signature: ()f
 * Required capability: FUSE_CAP_TEMP_SENSOR
 * --------------------------------------------------------------------------- */
float fuse_native_temp_get_reading(wasm_exec_env_t exec_env)
{
    fuse_module_desc_t *desc;
    wasm_module_inst_t  inst;

    inst = wasm_runtime_get_module_inst(exec_env);
    if (inst == NULL) {
        return 0.0f;
    }

    desc = fuse_module_find_by_inst(inst);

    if (desc == NULL) {
        fuse_log_write(&g_ctx.log_ctx, FUSE_INVALID_MODULE_ID, 2u,
                       "rogue exec_env: temp_get_reading");
    }

    if (!fuse_policy_check_cap(
            (desc != NULL) ? &desc->policy : NULL,
            FUSE_CAP_TEMP_SENSOR)) {
        policy_violation(desc, inst, "TEMP_SENSOR");
        return 0.0f;
    }

    if (g_ctx.hal.temp_get_reading == NULL) {
        return 0.0f;
    }

    return g_ctx.hal.temp_get_reading();
}

/* ---------------------------------------------------------------------------
 * fuse_native_timer_get_timestamp
 *
 * WASM signature: ()I
 * Required capability: FUSE_CAP_TIMER
 * --------------------------------------------------------------------------- */
uint64_t fuse_native_timer_get_timestamp(wasm_exec_env_t exec_env)
{
    fuse_module_desc_t *desc;
    wasm_module_inst_t  inst;

    inst = wasm_runtime_get_module_inst(exec_env);
    if (inst == NULL) {
        return 0u;
    }

    desc = fuse_module_find_by_inst(inst);

    if (desc == NULL) {
        fuse_log_write(&g_ctx.log_ctx, FUSE_INVALID_MODULE_ID, 2u,
                       "rogue exec_env: timer_get_timestamp");
    }

    if (!fuse_policy_check_cap(
            (desc != NULL) ? &desc->policy : NULL,
            FUSE_CAP_TIMER)) {
        policy_violation(desc, inst, "TIMER");
        return 0u;
    }

    if (g_ctx.hal.timer_get_timestamp == NULL) {
        return 0u;
    }

    return g_ctx.hal.timer_get_timestamp();
}

/* ---------------------------------------------------------------------------
 * fuse_native_camera_last_frame
 *
 * WASM signature: (*~)I
 *   WAMR auto-converts the app-side buffer pointer to a native pointer;
 *   max_len is the paired length from the '~' in the signature.
 *
 * Required capability: FUSE_CAP_CAMERA
 * Memory safety: validate the native pointer before passing to the HAL.
 * --------------------------------------------------------------------------- */
uint64_t fuse_native_camera_last_frame(wasm_exec_env_t exec_env,
                                       void *buf, uint32_t max_len)
{
    fuse_module_desc_t *desc;
    wasm_module_inst_t  inst;

    inst = wasm_runtime_get_module_inst(exec_env);
    if (inst == NULL) {
        return 0u;
    }

    desc = fuse_module_find_by_inst(inst);

    if (desc == NULL) {
        fuse_log_write(&g_ctx.log_ctx, FUSE_INVALID_MODULE_ID, 2u,
                       "rogue exec_env: camera_last_frame");
    }

    if (!fuse_policy_check_cap(
            (desc != NULL) ? &desc->policy : NULL,
            FUSE_CAP_CAMERA)) {
        policy_violation(desc, inst, "CAMERA");
        return 0u;
    }

    /* Reject null or zero-length buffers before any memory validation. */
    if ((buf == NULL) || (max_len == 0u)) {
        wasm_runtime_set_exception(inst,
                                   "FUSE camera: null or zero-length buffer");
        if (desc != NULL) {
            desc->state = FUSE_MODULE_STATE_TRAPPED;
        }
        return 0u;
    }

    /* Validate that buf[0..max_len-1] is within the module's linear memory. */
    if (!wasm_runtime_validate_native_addr(inst, buf, (uint64_t)max_len)) {
        /* WAMR sets an exception; also log the violation. */
        fuse_log_write(&g_ctx.log_ctx,
                       (desc != NULL) ? desc->id : FUSE_INVALID_MODULE_ID,
                       2u,
                       "SECURITY: camera buffer OOB");
        if (desc != NULL) {
            desc->state = FUSE_MODULE_STATE_TRAPPED;
        }
        return 0u;
    }

    if (g_ctx.hal.camera_last_frame == NULL) {
        return 0u;
    }

    return g_ctx.hal.camera_last_frame(buf, max_len);
}

/* ---------------------------------------------------------------------------
 * fuse_native_module_log_event
 *
 * WASM signature: (*~i)
 *   WAMR auto-converts the app-side ptr to a native pointer;
 *   len is the paired length from '~', level is a plain i32.
 *
 * Required capability: FUSE_CAP_LOG
 * Memory safety: validate native pointer before reading any bytes.
 * --------------------------------------------------------------------------- */
void fuse_native_module_log_event(wasm_exec_env_t exec_env,
                                  const char *ptr, uint32_t len,
                                  uint32_t level)
{
    fuse_module_desc_t *desc;
    wasm_module_inst_t  inst;
    char                safe_msg[FUSE_LOG_MSG_MAX];
    uint32_t            copy_len;

    inst = wasm_runtime_get_module_inst(exec_env);
    if (inst == NULL) {
        return;
    }

    desc = fuse_module_find_by_inst(inst);

    if (desc == NULL) {
        fuse_log_write(&g_ctx.log_ctx, FUSE_INVALID_MODULE_ID, 2u,
                       "rogue exec_env: module_log_event");
    }

    if (!fuse_policy_check_cap(
            (desc != NULL) ? &desc->policy : NULL,
            FUSE_CAP_LOG)) {
        policy_violation(desc, inst, "LOG");
        return;
    }

    /* Reject null or zero-length message buffers. */
    if ((ptr == NULL) || (len == 0u)) {
        return;
    }

    /* Validate that ptr[0..len-1] is within the module's linear memory. */
    if (!wasm_runtime_validate_native_addr(inst,
                                           (void *)(uintptr_t)ptr,
                                           (uint64_t)len)) {
        fuse_log_write(&g_ctx.log_ctx,
                       (desc != NULL) ? desc->id : FUSE_INVALID_MODULE_ID,
                       2u,
                       "SECURITY: log buffer OOB");
        if (desc != NULL) {
            desc->state = FUSE_MODULE_STATE_TRAPPED;
        }
        return;
    }

    /* Copy to a local buffer and NUL-terminate before forwarding to the log. */
    copy_len = (len < (FUSE_LOG_MSG_MAX - 1u))
               ? len
               : (FUSE_LOG_MSG_MAX - 1u);
    (void)memcpy(safe_msg, ptr, (size_t)copy_len);
    safe_msg[copy_len] = '\0';

    fuse_log_write(&g_ctx.log_ctx,
                   (desc != NULL) ? desc->id : FUSE_INVALID_MODULE_ID,
                   level,
                   safe_msg);
}
