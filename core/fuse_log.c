/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_log.c — Security-log ring-buffer implementation.
 *
 * The Host provides a fixed-size memory region at fuse_init() time.  FUSE
 * carves that region into an array of fuse_log_entry_t records and writes to
 * them in a circular fashion.  No dynamic allocation is performed.
 */

#include "fuse_internal.h"

#include <string.h>

/* g_ctx is defined in fuse_core.c and declared extern in fuse_internal.h. */

/* ---------------------------------------------------------------------------
 * fuse_log_write
 *
 * Writes a single log entry at write_idx and advances the index.  If the
 * buffer is full the oldest entry is silently overwritten (ring behaviour).
 *
 * Parameters:
 *   ctx  – pointer to the log context; must not be NULL and must have been
 *          initialised by fuse_core.c before this function is called.
 *   id   – module ID associated with the event (use FUSE_INVALID_MODULE_ID
 *          for runtime-level messages).
 *   level – log level: 0=DEBUG, 1=INFO, 2=FATAL.
 *   msg  – NUL-terminated message string; truncated to FUSE_LOG_MSG_MAX-1.
 * --------------------------------------------------------------------------- */
void fuse_log_write(fuse_log_ctx_t *ctx, fuse_module_id_t id,
                    uint32_t level, const char *msg)
{
    fuse_log_entry_t *entry;
    uint32_t          slot;

    /* Guard: context must exist and have at least one slot. */
    if ((ctx == NULL) || (ctx->entries == NULL) || (ctx->capacity == 0u)) {
        return;
    }

    if (msg == NULL) {
        return;
    }

    slot  = ctx->write_idx % ctx->capacity;
    entry = &ctx->entries[slot];

    /* Timestamp via HAL if available. */
    if (g_ctx.hal.timer_get_timestamp != NULL) {
        entry->timestamp_us = g_ctx.hal.timer_get_timestamp();
    } else {
        entry->timestamp_us = 0u;
    }

    entry->module_id = id;
    entry->level     = level;

    /* Safe copy: bound to actual string length to avoid reading past the end
     * of short strings (MISRA Directive 4.7).  Always NUL-terminate.
     * memcpy is used instead of strncpy (MISRA-C Rule 21.14 ban). */
    {
        size_t copy_len = strnlen(msg, (size_t)(FUSE_LOG_MSG_MAX - 1u));
        (void)memcpy(entry->message, msg, copy_len);
        entry->message[copy_len] = '\0';
    }

    /* Advance write pointer; wraps naturally via modulo on next call. */
    ctx->write_idx = (ctx->write_idx + 1u) % ctx->capacity;
}
