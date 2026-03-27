/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_policy.c — Module capability-policy checking.
 *
 * Every Module-to-FUSE HAL call is gated through fuse_policy_check_cap()
 * before any hardware access is performed.  This is the enforcement point
 * for the Capacity Bitmask security constraint.
 */

#include "fuse_internal.h"

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
