/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_hal_temp.h — Temperature sensor HAL group.
 *
 * Included by fuse.h when FUSE_HAL_ENABLE_TEMP_SENSOR is defined.
 * Contains only the public-facing group struct and the registration
 * declaration — no WAMR headers.
 */

#ifndef FUSE_HAL_TEMP_H
#define FUSE_HAL_TEMP_H

/**
 * Temperature sensor HAL group.
 * Enabled at compile time by defining FUSE_HAL_ENABLE_TEMP_SENSOR.
 */
typedef struct {
    /** Read the temperature sensor.  Returns degrees Celsius as a float. */
    float (*get_reading)(void);
} fuse_hal_temp_group_t;

/**
 * Register the temperature-sensor native symbol with WAMR.
 * Called once from fuse_init() after wasm_runtime_full_init().
 */
void fuse_hal_temp_register_natives(void);

#endif /* FUSE_HAL_TEMP_H */
