/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_event.c — ISR-safe event posting and clearing for the FUSE event
 *                activation system.
 */

#include "fuse_internal.h"
#include <stdatomic.h>

/* ---------------------------------------------------------------------------
 * fuse_post_event
 *
 * ISR-safe: uses only atomic_fetch_or, no locks, no log writes, no WAMR calls.
 * --------------------------------------------------------------------------- */
fuse_stat_t fuse_post_event(uint32_t event_id)
{
    uint32_t i;
    uint32_t bit;

    if (!g_ctx.initialized) {
        return FUSE_ERR_NOT_INITIALIZED;
    }
    if (event_id >= 32u) {
        return FUSE_ERR_INVALID_ARG;
    }

    bit = (1u << event_id);

    /*
     * For each module slot that subscribes to this event, atomically set
     * the event bit in its latch.  The slot's event_subscribe mask is read
     * non-atomically; a data race here (concurrent load/unload) is benign
     * because fuse_module_load zeroes the slot (including event_latch) before
     * setting in_use, and fuse_module_unload zeroes the slot after tearing
     * down WAMR resources.  An ISR that posts an event during a load/unload
     * window either sees the old or the new subscribe mask; both outcomes are
     * safe because the latch is also zeroed on load/unload.
     *
     * The acquire fence before the loop pairs with the release fence in
     * fuse_module_load() before slot->in_use = true, ensuring we observe a
     * fully initialised (zeroed) slot, including event_latch, before acting on it.
     */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    for (i = 0u; i < FUSE_MAX_MODULES; i++) {
        if (g_ctx.modules[i].in_use &&
            (g_ctx.modules[i].policy.event_subscribe & bit)) {
            atomic_fetch_or_explicit(&g_ctx.modules[i].event_latch, bit, memory_order_relaxed);
        }
    }

    return FUSE_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * fuse_clear_event
 * --------------------------------------------------------------------------- */
fuse_stat_t fuse_clear_event(uint32_t event_id)
{
    uint32_t i;
    uint32_t clear_mask;

    if (!g_ctx.initialized) {
        return FUSE_ERR_NOT_INITIALIZED;
    }
    if (event_id >= 32u) {
        return FUSE_ERR_INVALID_ARG;
    }

    clear_mask = ~(1u << event_id);

    for (i = 0u; i < FUSE_MAX_MODULES; i++) {
        if (g_ctx.modules[i].in_use) {
            atomic_fetch_and_explicit(&g_ctx.modules[i].event_latch, clear_mask, memory_order_relaxed);
        }
    }

    return FUSE_SUCCESS;
}
