/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_hal_event.c — WASM native bridge for module-side event posting.
 *
 * Exposes one native symbol to modules:
 *   fuse_event_post(event_id: i32) -> void
 *
 * Access is gated by FUSE_CAP_EVENT_POST.  A module without this capability
 * that calls fuse_event_post() is trapped with a policy violation.
 *
 * This group is always compiled (no FUSE_HAL_ENABLE_EVENT guard) because
 * the event system is a core FUSE facility, not an optional hardware group.
 */

#include "fuse_hal_event.h"
#include "../fuse_internal.h"
#include "wasm_export.h"
#include "fuse.h"

/* ---------------------------------------------------------------------------
 * fuse_native_event_post — WASM-callable native for fuse_event_post()
 * --------------------------------------------------------------------------- */
static void fuse_native_event_post(wasm_exec_env_t exec_env,
                                   uint32_t        event_id)
{
    wasm_module_inst_t  inst;
    fuse_module_desc_t *desc;

    inst = wasm_runtime_get_module_inst(exec_env);
    desc = fuse_module_find_by_inst(inst);

    if (desc == NULL) {
        fuse_log_write(&g_ctx.log_ctx, FUSE_INVALID_MODULE_ID, 2u,
                       "event_post: unknown module instance");
        wasm_runtime_set_exception(inst, "FUSE: rogue exec_env");
        return;
    }

    /* Gate on FUSE_CAP_EVENT_POST capability. */
    if (!(desc->policy.capabilities & FUSE_CAP_EVENT_POST)) {
        fuse_policy_violation(desc, inst, "EVENT_POST");
        return;
    }

    /* Validate event_id range (fuse_post_event also checks, but be explicit). */
    if (event_id >= 32u) {
        fuse_policy_violation(desc, inst, "EVENT_POST_OOB");
        return;
    }

    /* Post through the public ISR-safe API. */
    if (fuse_post_event(event_id) != FUSE_SUCCESS) {
        fuse_policy_violation(desc, inst, "EVENT_POST_FAILED");
    }
}

/* ---------------------------------------------------------------------------
 * NativeSymbol table
 * WAMR signature: "(i)" = one i32 arg, returns void.
 * --------------------------------------------------------------------------- */
static NativeSymbol s_event_symbols[] = {
    { "fuse_event_post", fuse_native_event_post, "(i)", NULL }
};

void fuse_hal_event_register_natives(void)
{
    wasm_runtime_register_natives(
        "env",
        s_event_symbols,
        sizeof(s_event_symbols) / sizeof(s_event_symbols[0]));
}
