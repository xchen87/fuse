# Module to FUSE API
- **Constraint** All Module to FUSE API calls are checked against Module Policy before execution.
- Module to FUSE calls are either HAL API (hardware access) or Log API (security log).

## C Module Boilerplate
*(freestanding — no libc, no WASI; compiled with `--target=wasm32-unknown-unknown -nostdlib`)*

```c
/* Declare each import with matching attributes: */
__attribute__((import_module("env"), import_name("temp_get_reading")))
extern float temp_get_reading(void);

__attribute__((import_module("env"), import_name("timer_get_timestamp")))
extern unsigned long long timer_get_timestamp(void);

__attribute__((import_module("env"), import_name("camera_last_frame")))
extern unsigned long long camera_last_frame(void *buf, unsigned int max_len);

__attribute__((import_module("env"), import_name("module_log_event")))
extern void module_log_event(const char *ptr, unsigned int len, unsigned int level);

__attribute__((import_module("env"), import_name("fuse_event_post")))
extern void fuse_event_post(unsigned int event_id);

/* Required export: */
__attribute__((export_name("module_step"))) void module_step(void) { /* no infinite loops */ }
/* Optional exports: */
__attribute__((export_name("module_init")))   void module_init(void)   { }
__attribute__((export_name("module_deinit"))) void module_deinit(void) { }
```
Note: WASM linear memory is zero-initialised at startup — static/global arrays start zeroed.
Use basic C integer arithmetic and array indexing freely. Do NOT call libc I/O (printf, fwrite, etc.).

## HAL API

### Temperature Sensor
#### `float temp_get_reading(void)`
- Requires: `FUSE_CAP_TEMP_SENSOR` (0x01)
- Returns: Celsius as float

### Timer
#### `uint64_t timer_get_timestamp(void)`
- Requires: `FUSE_CAP_TIMER` (0x02)
- Returns: elapsed microseconds (monotonic)

### Camera
#### `uint64_t camera_last_frame(void *buf, uint32_t max_len)`
- Requires: `FUSE_CAP_CAMERA` (0x04)
- Copies latest frame into `buf[0..max_len-1]` (within module linear memory)
- Returns: actual bytes written; 0 if no frame available or buffer invalid

## Log API
#### `void module_log_event(const char *ptr, uint32_t len, uint32_t level)`
- Requires: `FUSE_CAP_LOG` (0x08)
- `ptr`: source buffer in module linear memory; `len`: byte count (not NUL-terminated)
- `level`: 0=DEBUG, 1=INFO, 2=FATAL
- Messages longer than `FUSE_LOG_MSG_MAX` (128) are truncated

## Event API
#### `void fuse_event_post(uint32_t event_id)`
- Requires: `FUSE_CAP_EVENT_POST` (0x10)
- `event_id`: event identifier, 0-31 (must match an ID subscribed by at least one other module)
- Sets the event bit in the latch of every RUNNING module whose `policy.event_subscribe` includes `event_id`
- Calling with `event_id >= 32` triggers a policy violation trap on the calling module
- Calling without `FUSE_CAP_EVENT_POST` in policy triggers a policy violation trap
- Use this to signal downstream pipeline stages (e.g. producer module triggers consumer module)
