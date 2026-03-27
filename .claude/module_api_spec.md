# Module to FUSE API
- **Constraint** All Module to FUSE API calls need be be checked against Module Policies
- Module to FUSE Calls can categorized as either a HAL API that intents to access hardware resources, or a Log API to log module's critical event to FUSE security log.

## HAL API

### Temperature Sensor
#### `float temp_get_reading()`
- Description: Read and return current temperature reading from temp sensors on host
- Return: float temperature reading

### Timer
#### `uint64 timer_get_timestamp()`
- Description: Returns elapsed time in microseconds.
- Return: uint64

### Camera
#### `uint64 camera_last_frame(buffer_ptr, max_len)`
- Description: Get the latest camera frame data
- Return: uint64 (actual bytes of the lastest frame buffer)
- Parameters:
    - buffer_ptr: destination in module memory
    - max_len: buffer size to contain fame data

## Log API
### `module_log_event(ptr, len, level)`
- Description: sends a log record to the fuse security log
- Parameters:
    - ptr: source log buffer
    - len: source log size
    - level: (uint32, 0:DEBUG, 1:INFO, 2:FATAL)
