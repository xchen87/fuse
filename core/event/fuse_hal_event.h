/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_hal_event.h — Module-side event posting HAL group.
 *
 * Exposes fuse_event_post() as a WASM native import so modules can signal
 * downstream pipeline stages.  Gated by FUSE_CAP_EVENT_POST in policy.
 *
 * This group has no host hardware callback (no entry in fuse_hal_t) — it
 * routes directly to fuse_post_event() inside the FUSE core.
 */

#ifndef FUSE_HAL_EVENT_H
#define FUSE_HAL_EVENT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called once from fuse_init() after wasm_runtime_full_init(). */
void fuse_hal_event_register_natives(void);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_HAL_EVENT_H */
