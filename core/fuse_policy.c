/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_policy.c — Module capability-policy checking and violation handling.
 *
 * Every Module-to-FUSE HAL call is gated through fuse_policy_check_cap()
 * before any hardware access is performed.  This is the enforcement point
 * for the Capacity Bitmask security constraint.
 *
 * fuse_policy_violation() is the shared enforcement action called by all
 * HAL group bridge files when a capability check fails.
 */

#include "fuse_internal.h"

#include <stdio.h>

/* ---------------------------------------------------------------------------
 * fuse_policy_check_cap
 *
 * Returns true if cap_bit is set in policy->capabilities, false otherwise.
 *
 * This function does not log, trap, or raise exceptions — the calling native
 * function (in fuse_hal.c) is responsible for handling a false return.
 *
 * Parameters:
 *   policy  – must not be NULL.
 *   cap_bit – one of the FUSE_CAP_* bitmask constants.
 * --------------------------------------------------------------------------- */
bool fuse_policy_check_cap(const fuse_policy_t *policy, uint32_t cap_bit)
{
    if (policy == NULL) {
        return false;
    }
    return ((policy->capabilities & cap_bit) != 0u);
}

/* ---------------------------------------------------------------------------
 * fuse_policy_violation
 *
 * Logs a FATAL security event, sets a WASM exception so the calling
 * wasm_runtime_call_wasm() returns false, and transitions the module to
 * TRAPPED state.  Shared by all HAL group bridge implementations.
 * --------------------------------------------------------------------------- */
void fuse_policy_violation(fuse_module_desc_t *desc,
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

    if (inst != NULL) {
        wasm_runtime_set_exception(inst, "FUSE policy violation");
    }

    if (desc != NULL) {
        desc->state = FUSE_MODULE_STATE_TRAPPED;
    }
}
