/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_hal_camera.c — Camera native-function bridge.
 *
 * Compiled only when FUSE_HAL_ENABLE_CAMERA is defined.
 * Implements fuse_native_camera_last_frame() and registers it with WAMR.
 */

#ifdef FUSE_HAL_ENABLE_CAMERA

#include "fuse_hal_camera.h"
#include "../fuse_internal.h"

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
static uint32_t fuse_native_camera_last_frame(wasm_exec_env_t exec_env,
                                              void *buf, uint32_t max_len)
{
    fuse_module_desc_t *desc;
    wasm_module_inst_t  inst;
    uint64_t            frame_bytes;

    desc = fuse_hal_resolve_desc(exec_env, &inst);
    if (desc == NULL) {
        return 0u;
    }

    if (!fuse_policy_check_cap(&desc->policy, FUSE_CAP_CAMERA)) {
        fuse_policy_violation(desc, inst, "CAMERA");
        return 0u;
    }

    /* Reject null or zero-length buffers before any memory validation. */
    if ((buf == NULL) || (max_len == 0u)) {
        fuse_log_write(&g_ctx.log_ctx, desc->id, 2u,
                       "SECURITY: camera null or zero-length buffer");
        wasm_runtime_set_exception(inst,
                                   "FUSE camera: null or zero-length buffer");
        desc->state = FUSE_MODULE_STATE_TRAPPED;
        return 0u;
    }

    /* Validate that buf[0..max_len-1] is within the module's linear memory. */
    if (!wasm_runtime_validate_native_addr(inst, buf, (uint64_t)max_len)) {
        fuse_log_write(&g_ctx.log_ctx, desc->id, 2u,
                       "SECURITY: camera buffer OOB");
        desc->state = FUSE_MODULE_STATE_TRAPPED;
        return 0u;
    }

    if (g_ctx.hal.camera.last_frame == NULL) {
        return 0u;
    }

    frame_bytes = g_ctx.hal.camera.last_frame(buf, max_len);
    /* HAL returns uint64_t; clamp to the buffer size before narrowing. */
    if (frame_bytes > (uint64_t)max_len) {
        frame_bytes = (uint64_t)max_len;
    }
    return (uint32_t)frame_bytes;
}

/* ---------------------------------------------------------------------------
 * NativeSymbol table for this group (file-local)
 * --------------------------------------------------------------------------- */
static NativeSymbol s_camera_symbols[] = {
    {
        "camera_last_frame",
        (void *)fuse_native_camera_last_frame,
        "(*~)I",
        NULL
    }
};

/* ---------------------------------------------------------------------------
 * fuse_hal_camera_register_natives
 * --------------------------------------------------------------------------- */
void fuse_hal_camera_register_natives(void)
{
    wasm_runtime_register_natives(
        "env",
        s_camera_symbols,
        (uint32_t)(sizeof(s_camera_symbols) / sizeof(s_camera_symbols[0])));
}

#endif /* FUSE_HAL_ENABLE_CAMERA */
