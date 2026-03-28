/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_hal_timer.h — Timer HAL group.
 *
 * Included by fuse.h when FUSE_HAL_ENABLE_TIMER is defined.
 * Contains only the public-facing group struct and the registration
 * declaration — no WAMR headers.
 */

#ifndef FUSE_HAL_TIMER_H
#define FUSE_HAL_TIMER_H

#include <stdint.h>

/**
 * Timer HAL group.
 * Enabled at compile time by defining FUSE_HAL_ENABLE_TIMER.
 */
typedef struct {
    /** Return elapsed time in microseconds (monotonic). */
    uint64_t (*get_timestamp)(void);
} fuse_hal_timer_group_t;

/**
 * Register the timer native symbol with WAMR.
 * Called once from fuse_init() after wasm_runtime_full_init().
 */
void fuse_hal_timer_register_natives(void);

#endif /* FUSE_HAL_TIMER_H */
