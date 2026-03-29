/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_hal_timer.c — Timer native-function bridge.
 *
 * Compiled only when FUSE_HAL_ENABLE_TIMER is defined.
 * Implements fuse_native_timer_get_timestamp() and registers it with WAMR.
 */

#ifdef FUSE_HAL_ENABLE_TIMER

#include "fuse_hal_timer.h"
#include "../fuse_internal.h"

/* ---------------------------------------------------------------------------
 * fuse_native_timer_get_timestamp
 *
 * WASM signature: ()I
 * Required capability: FUSE_CAP_TIMER
 * --------------------------------------------------------------------------- */
static uint64_t fuse_native_timer_get_timestamp(wasm_exec_env_t exec_env)
{
    fuse_module_desc_t *desc;
    wasm_module_inst_t  inst;

    desc = fuse_hal_resolve_desc(exec_env, &inst);
    if (desc == NULL) {
        return 0u;
    }

    if (!fuse_policy_check_cap(&desc->policy, FUSE_CAP_TIMER)) {
        fuse_policy_violation(desc, inst, "TIMER");
        return 0u;
    }

    if (g_ctx.hal.timer.get_timestamp == NULL) {
        return 0u;
    }

    return g_ctx.hal.timer.get_timestamp();
}

/* ---------------------------------------------------------------------------
 * NativeSymbol table for this group (file-local)
 * --------------------------------------------------------------------------- */
static NativeSymbol s_timer_symbols[] = {
    {
        "timer_get_timestamp",
        (void *)fuse_native_timer_get_timestamp,
        "()I",
        NULL
    }
};

/* ---------------------------------------------------------------------------
 * fuse_hal_timer_register_natives
 * --------------------------------------------------------------------------- */
void fuse_hal_timer_register_natives(void)
{
    wasm_runtime_register_natives(
        "env",
        s_timer_symbols,
        (uint32_t)(sizeof(s_timer_symbols) / sizeof(s_timer_symbols[0])));
}

#endif /* FUSE_HAL_ENABLE_TIMER */
