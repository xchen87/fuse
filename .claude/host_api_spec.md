# FUSE Host API Specification
***Host* uses these APIs to manage *FUSE* and *Module***

## HAL Callback Structure
The host provides hardware implementations at `fuse_init()` time. All fields may be NULL.
```c
typedef struct {
    float    (*temp_get_reading)(void);                          /* FUSE_CAP_TEMP_SENSOR */
    uint64_t (*timer_get_timestamp)(void);                       /* FUSE_CAP_TIMER — µs monotonic */
    uint64_t (*camera_last_frame)(void *buf, uint32_t max_len); /* FUSE_CAP_CAMERA — returns bytes written */
    void     (*quota_arm)(fuse_module_id_t mid, uint32_t us);   /* arm one-shot timer; fire → fuse_quota_expired() */
    void     (*quota_cancel)(fuse_module_id_t mid);             /* cancel armed timer on normal step return */
} fuse_hal_t;
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
