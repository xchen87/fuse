# FUSE Host API Specification
***Host* uses these APIs to manage *FUSE* and *Module***

## HAL Group Structure
Hardware APIs are organized into compile-time-conditional groups. The host provides group structs at
`fuse_init()` time. Only groups enabled via `FUSE_HAL_ENABLE_*` compile flags appear in `fuse_hal_t`.

```c
/* Group sub-structs (each defined in core/<group>/fuse_hal_<group>.h) */
typedef struct { float    (*get_reading)(void); }                       fuse_hal_temp_group_t;
typedef struct { uint64_t (*get_timestamp)(void); }                     fuse_hal_timer_group_t;
typedef struct { uint64_t (*last_frame)(void *buf, uint32_t max_len); } fuse_hal_camera_group_t;

/* Quota callbacks — always present, not a hardware group */
typedef void (*fuse_hal_quota_arm_fn)(fuse_module_id_t mid, uint32_t us);
typedef void (*fuse_hal_quota_cancel_fn)(fuse_module_id_t mid);

/* Compile-time conditional HAL struct */
typedef struct {
#ifdef FUSE_HAL_ENABLE_TEMP_SENSOR
    fuse_hal_temp_group_t    temp;    /* FUSE_CAP_TEMP_SENSOR */
#endif
#ifdef FUSE_HAL_ENABLE_TIMER
    fuse_hal_timer_group_t   timer;   /* FUSE_CAP_TIMER — µs monotonic */
#endif
#ifdef FUSE_HAL_ENABLE_CAMERA
    fuse_hal_camera_group_t  camera;  /* FUSE_CAP_CAMERA — returns bytes written */
#endif
    fuse_hal_quota_arm_fn    quota_arm;    /* arm one-shot timer; fire → fuse_quota_expired() */
    fuse_hal_quota_cancel_fn quota_cancel; /* cancel armed timer on normal step return */
} fuse_hal_t;
```

**Log group**: `module_log_event` is always registered with WAMR and writes to FUSE's internal ring
buffer. It has no host hardware callback and does not appear in `fuse_hal_t`. Controlled only by
`FUSE_CAP_LOG` in module policy.

**Event group**: `fuse_event_post` is always registered with WAMR and routes to `fuse_post_event()`
inside the FUSE core. It has no host hardware callback and does not appear in `fuse_hal_t`. Controlled
by `FUSE_CAP_EVENT_POST` in module policy. Modules use it to signal downstream pipeline stages.

**HAL group registration**: after `wasm_runtime_full_init()` in `fuse_init()`, each enabled group's
native symbols are registered via `fuse_hal_<group>_register_natives()` (defined in `core/<group>/`).
The log and event groups are always registered unconditionally.

**Initialization example** (timer + camera only):
```c
fuse_hal_t hal;
memset(&hal, 0, sizeof(hal));
hal.timer.get_timestamp = my_timer_fn;
hal.camera.last_frame   = my_camera_fn;
hal.quota_arm           = my_quota_arm_fn;
hal.quota_cancel        = my_quota_cancel_fn;
fuse_init(mod_mem, mod_mem_sz, log_mem, log_mem_sz, &hal);
```

## *FUSE* Management APIs

### `fuse_init`
```c
fuse_stat_t fuse_init(void *module_memory, size_t module_memory_size,
                      void *log_memory,    size_t log_memory_size,
                      const fuse_hal_t *hal);
```
- Initialises WAMR with the provided memory pool. Call exactly once before any other API.
- Returns: `FUSE_SUCCESS`, `FUSE_ERR_INVALID_ARG`, `FUSE_ERR_ALREADY_INITIALIZED`

### `fuse_stop`
```c
fuse_stat_t fuse_stop(void);
```
- Transitions all RUNNING modules → PAUSED. No steps may execute until `fuse_restart()`.
- Returns: `FUSE_SUCCESS`, `FUSE_ERR_NOT_INITIALIZED`

### `fuse_restart`
```c
fuse_stat_t fuse_restart(void);
```
- Resumes the runtime after `fuse_stop()`. Modules return to RUNNING.
- Returns: `FUSE_SUCCESS`, `FUSE_ERR_NOT_INITIALIZED`

## *Module* Management APIs

### `fuse_module_load`
```c
fuse_stat_t fuse_module_load(const uint8_t *module_buf, uint32_t module_size,
                             const fuse_policy_t *policy,
                             fuse_module_id_t *out_id);
```
- Loads an AOT binary. `module_buf` must remain valid for the module's lifetime (WAMR does not copy it).
- `out_id` receives the assigned ID on success.
- Returns: `FUSE_SUCCESS`, `FUSE_ERR_INVALID_ARG`, `FUSE_ERR_NOT_INITIALIZED`, `FUSE_ERR_MODULE_LIMIT`, `FUSE_ERR_MODULE_LOAD_FAILED`

### `fuse_module_start`
```c
fuse_stat_t fuse_module_start(fuse_module_id_t id);
```
- Valid from LOADED or PAUSED. Calls `module_init()` on first start if exported, then → RUNNING.
- Returns: `FUSE_SUCCESS`, `FUSE_ERR_INVALID_ARG`, `FUSE_ERR_NOT_INITIALIZED`, `FUSE_ERR_MODULE_NOT_FOUND`

### `fuse_module_pause`
```c
fuse_stat_t fuse_module_pause(fuse_module_id_t id);
```
- Valid from RUNNING. Transitions → PAUSED.
- Returns: `FUSE_SUCCESS`, `FUSE_ERR_INVALID_ARG`, `FUSE_ERR_MODULE_NOT_FOUND`

### `fuse_module_stat`
```c
fuse_stat_t fuse_module_stat(fuse_module_id_t id, fuse_module_state_t *out_state);
```
- Queries current state. States: `LOADED=0 RUNNING=1 PAUSED=2 TRAPPED=3 QUOTA_EXCEEDED=4 UNLOADED=5`
- Returns: `FUSE_SUCCESS`, `FUSE_ERR_INVALID_ARG`, `FUSE_ERR_MODULE_NOT_FOUND`

### `fuse_module_unload`
```c
fuse_stat_t fuse_module_unload(fuse_module_id_t id);
```
- Calls `module_deinit()` if applicable (skipped when TRAPPED or QUOTA_EXCEEDED), then reclaims the slot.
- Returns: `FUSE_SUCCESS`, `FUSE_ERR_INVALID_ARG`, `FUSE_ERR_MODULE_NOT_FOUND`

### `fuse_module_run_step`
```c
fuse_stat_t fuse_module_run_step(fuse_module_id_t id);
```
- Executes one call to `module_step()`. Arms quota timer before call, cancels on normal return.
- Module must be in RUNNING state.
- Returns: `FUSE_SUCCESS`, `FUSE_ERR_INVALID_ARG`, `FUSE_ERR_NOT_INITIALIZED`, `FUSE_ERR_QUOTA_EXCEEDED`, `FUSE_ERR_MODULE_TRAP`

### `fuse_quota_expired`
```c
void fuse_quota_expired(fuse_module_id_t id);
```
- Called from the quota timer ISR when a step exceeds its wall-clock budget.
- ISR-safe: only calls `wasm_runtime_terminate()` + atomic fence. Sets module → QUOTA_EXCEEDED.

### `fuse_tick`
```c
uint32_t fuse_tick(void);
```
- Iterates all RUNNING module slots and calls `fuse_module_run_step()` on each.
- Modules that return `FUSE_ERR_INTERVAL_NOT_ELAPSED` are silently skipped (not yet due).
- Modules that trap or exceed quota have their state updated as normal; iteration continues.
- Returns a bitmask of module IDs that completed a step with `FUSE_SUCCESS` this tick (bit N = module id N ran). Returns 0 if not initialized, runtime stopped, or no modules were due.
- Typical usage (bare-metal superloop or single RTOS scheduler task):
  ```c
  while (1) { usleep(1000); fuse_tick(); }
  ```
- Returns: `uint32_t` bitmask (valid for module IDs 0–31; `FUSE_MAX_MODULES` ≤ 32 enforced by `_Static_assert`)

### `fuse_policy_from_bin`
```c
fuse_stat_t fuse_policy_from_bin(const uint8_t *buf, uint32_t len,
                                  fuse_policy_t *out_policy);
```
- Deserialises a 32-byte little-endian policy binary (`*_policy.bin`) into a `fuse_policy_t`.
- Wire format: 8 × uint32_t little-endian, matching `fuse_policy_t` in-memory layout on all WAMR-supported little-endian targets.
- Use this for **dynamic module loading** when policy is delivered at runtime rather than compiled in.
- Returns: `FUSE_SUCCESS`, `FUSE_ERR_INVALID_ARG` (NULL pointer or `len != sizeof(fuse_policy_t)`)

**Dynamic module loading pattern**:
```c
/* Read policy.bin delivered at runtime */
uint8_t policy_buf[sizeof(fuse_policy_t)];
/* ... fill policy_buf from file/uplink/OTA ... */
fuse_policy_t policy;
fuse_stat_t st = fuse_policy_from_bin(policy_buf, sizeof(policy_buf), &policy);
if (st != FUSE_SUCCESS) { /* handle error */ }

/* Load the module binary */
fuse_module_id_t id;
st = fuse_module_load(module_buf, module_size, &policy, &id);
```

**app_config.json dual-mode**: `tools/gen_app_config.py` supports two forms:
- **With `modules` section**: generates both `fuse_app_config.h` (compile-time macros) and per-module `*_policy.bin` files — use `FUSE_POLICY_*` macros for static deployments.
- **Without `modules` section** (platform-only config): generates only `fuse_app_config.h` with pool sizes and HAL flags — modules are loaded fully dynamically at runtime via `fuse_policy_from_bin()`.

### `fuse_post_event`
```c
fuse_stat_t fuse_post_event(uint32_t event_id);
```
- Sets the event bit in the event latch of every RUNNING module whose `policy.event_subscribe` mask includes `event_id`.
- On the next `fuse_tick()` call any such module with `FUSE_ACTIVATION_EVENT` will execute one step.
- ISR-safe: uses only `atomic_fetch_or` operations — no locks, no log writes, no WAMR calls.
- Returns: `FUSE_SUCCESS`, `FUSE_ERR_INVALID_ARG` (event_id >= 32 or not initialised)

**Event-chaining pattern** — a module can trigger a downstream pipeline stage:
```c
/* Module A (producer) — calls fuse_event_post via WASM native import */
/* Host side — post an event directly from e.g. a camera frame DMA ISR  */
fuse_post_event(FUSE_EVENT_CAMERA_FRAME_READY);  /* non-blocking, ISR-safe */
```

### `fuse_clear_event`
```c
fuse_stat_t fuse_clear_event(uint32_t event_id);
```
- Removes the event bit from every module's event latch.
- Useful for draining stale events during shutdown or reset sequences.
- Returns: `FUSE_SUCCESS`, `FUSE_ERR_INVALID_ARG` (event_id >= 32 or not initialised)
