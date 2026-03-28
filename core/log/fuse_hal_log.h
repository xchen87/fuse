/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_hal_log.h — Log HAL group (always present).
 *
 * The log group writes to FUSE's internal ring buffer — it has no host
 * hardware callback and does not contribute a member to fuse_hal_t.
 * It is always compiled and registered regardless of which hardware groups
 * are enabled.
 *
 * This header is included only by fuse_internal.h (not by fuse.h).
 */

#ifndef FUSE_HAL_LOG_H
#define FUSE_HAL_LOG_H

/**
 * Register the log native symbol with WAMR.
 * Called unconditionally from fuse_init() after wasm_runtime_full_init().
 */
void fuse_hal_log_register_natives(void);

#endif /* FUSE_HAL_LOG_H */
