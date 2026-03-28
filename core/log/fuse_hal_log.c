/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_hal_log.c — Log native-function bridge (always compiled).
 *
 * module_log_event writes to FUSE's internal security-log ring buffer.
 * It has no host hardware callback and is always registered with WAMR,
 * independent of which hardware HAL groups are enabled.
 */

#include "fuse_hal_log.h"
#include "../fuse_internal.h"

#include <string.h>

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
static void fuse_native_module_log_event(wasm_exec_env_t exec_env,
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
        wasm_runtime_set_exception(inst, "FUSE: rogue exec_env");
        return;
    }

    if (!fuse_policy_check_cap(&desc->policy, FUSE_CAP_LOG)) {
        fuse_policy_violation(desc, inst, "LOG");
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
        fuse_log_write(&g_ctx.log_ctx, desc->id, 2u,
                       "SECURITY: log buffer OOB");
        desc->state = FUSE_MODULE_STATE_TRAPPED;
        return;
    }

    /* Copy to a local buffer and NUL-terminate before forwarding to the log. */
    copy_len = (len < (FUSE_LOG_MSG_MAX - 1u))
               ? len
               : (FUSE_LOG_MSG_MAX - 1u);
    (void)memcpy(safe_msg, ptr, (size_t)copy_len);
    safe_msg[copy_len] = '\0';

    fuse_log_write(&g_ctx.log_ctx, desc->id, level, safe_msg);
}

/* ---------------------------------------------------------------------------
 * NativeSymbol table for this group (file-local)
 * --------------------------------------------------------------------------- */
static NativeSymbol s_log_symbols[] = {
    {
        "module_log_event",
        (void *)fuse_native_module_log_event,
        "(*~i)",
        NULL
    }
};

/* ---------------------------------------------------------------------------
 * fuse_hal_log_register_natives
 * --------------------------------------------------------------------------- */
void fuse_hal_log_register_natives(void)
{
    wasm_runtime_register_natives(
        "env",
        s_log_symbols,
        (uint32_t)(sizeof(s_log_symbols) / sizeof(s_log_symbols[0])));
}
