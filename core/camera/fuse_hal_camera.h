/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_hal_camera.h — Camera HAL group.
 *
 * Included by fuse.h when FUSE_HAL_ENABLE_CAMERA is defined.
 * Contains only the public-facing group struct and the registration
 * declaration — no WAMR headers.
 */

#ifndef FUSE_HAL_CAMERA_H
#define FUSE_HAL_CAMERA_H

#include <stdint.h>

/**
 * Camera HAL group.
 * Enabled at compile time by defining FUSE_HAL_ENABLE_CAMERA.
 */
typedef struct {
    /**
     * Copy the most recent camera frame into buf[0..max_len-1].
     * Returns the actual number of bytes written; 0 if no frame available
     * or buffer is invalid.
     */
    uint64_t (*last_frame)(void *buf, uint32_t max_len);
} fuse_hal_camera_group_t;

/**
 * Register the camera native symbol with WAMR.
 * Called once from fuse_init() after wasm_runtime_full_init().
 */
void fuse_hal_camera_register_natives(void);

#endif /* FUSE_HAL_CAMERA_H */
