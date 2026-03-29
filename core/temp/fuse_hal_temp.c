/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_hal_temp.c — Temperature sensor native-function bridge.
 *
 * Compiled only when FUSE_HAL_ENABLE_TEMP_SENSOR is defined.
 * Implements fuse_native_temp_get_reading() and registers it with WAMR.
 */

#ifdef FUSE_HAL_ENABLE_TEMP_SENSOR

#include "fuse_hal_temp.h"
#include "../fuse_internal.h"

/* ---------------------------------------------------------------------------
 * fuse_native_temp_get_reading
 *
 * WASM signature: ()f
 * Required capability: FUSE_CAP_TEMP_SENSOR
 * --------------------------------------------------------------------------- */
static float fuse_native_temp_get_reading(wasm_exec_env_t exec_env)
{
    fuse_module_desc_t *desc;
    wasm_module_inst_t  inst;

    desc = fuse_hal_resolve_desc(exec_env, &inst);
    if (desc == NULL) {
        return 0.0f;
    }

    if (!fuse_policy_check_cap(&desc->policy, FUSE_CAP_TEMP_SENSOR)) {
        fuse_policy_violation(desc, inst, "TEMP_SENSOR");
        return 0.0f;
    }

    if (g_ctx.hal.temp.get_reading == NULL) {
        return 0.0f;
    }

    return g_ctx.hal.temp.get_reading();
}

/* ---------------------------------------------------------------------------
 * NativeSymbol table for this group (file-local)
 * --------------------------------------------------------------------------- */
static NativeSymbol s_temp_symbols[] = {
    {
        "temp_get_reading",
        (void *)fuse_native_temp_get_reading,
        "()f",
        NULL
    }
};

/* ---------------------------------------------------------------------------
 * fuse_hal_temp_register_natives
 * --------------------------------------------------------------------------- */
void fuse_hal_temp_register_natives(void)
{
    wasm_runtime_register_natives(
        "env",
        s_temp_symbols,
        (uint32_t)(sizeof(s_temp_symbols) / sizeof(s_temp_symbols[0])));
}

#endif /* FUSE_HAL_ENABLE_TEMP_SENSOR */
