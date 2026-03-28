# Application Demo Workflow

This document defines the step-by-step process for adding a new application demo to FUSE.

## Overview

Each demo lives under `demos/<name>/` and has a single `app_config.json` as its authoritative
deployment description. A reviewer reading only `app_config.json` can understand exactly what
hardware the application uses and what each module is permitted to do.

---

## Step 1 — Write `app_config.json`

Create `demos/<name>/app_config.json`. This is the first artefact a reviewer sees.

```json
{
    "application": {
        "name": "<name>",
        "description": "<human description>",
        "max_modules": <N>,
        "wamr_pool_bytes": <bytes>,
        "log_pool_bytes": <bytes>,
        "hal_groups": ["timer", "camera"]
    },
    "modules": [
        {
            "name": "<module_name>",
            "binary": "out/<module_name>.aot",
            "policy": {
                "capabilities": ["TIMER", "CAMERA", "LOG"],
                "memory_pages_max": <pages>,
                "stack_size": <bytes>,
                "heap_size": <bytes>,
                "cpu_quota_us": <us>,
                "step_interval_us": <us>
            }
        }
    ]
}
```

**Rules:**
- `hal_groups` valid values: `"temp_sensor"`, `"timer"`, `"camera"` (never `"log"` — always present)
- `capabilities` must be subset of `hal_groups ∪ {LOG}`
- `max_modules` ≤ 8
- Module `name` must be a valid C identifier

**Use `@scripter`** to validate the JSON and generate artifacts:
```bash
python3 tools/gen_app_config.py --input demos/<name>/app_config.json --output-dir /tmp/review
cat /tmp/review/fuse_app_config.h   # review generated constants
```

---

## Step 2 — Write the WASM Module (use `@developer`)

Create `demos/<name>/module/<module_name>.c` following the module contract:

```c
/* Freestanding — no libc, no WASI */
__attribute__((import_module("env"), import_name("timer_get_timestamp")))
extern unsigned long long timer_get_timestamp(void);

__attribute__((export_name("module_init")))   void module_init(void)   { }
__attribute__((export_name("module_step")))   void module_step(void)   { /* no infinite loops */ }
__attribute__((export_name("module_deinit"))) void module_deinit(void) { }
```

Only import HAL APIs that are in `capabilities` (and therefore in `hal_groups`).

---

## Step 3 — Write the Host Application (use `@developer`)

Create `demos/<name>/host/main.c`.

Include the generated config header for platform constants:
```c
#ifdef FUSE_APP_WAMR_POOL_BYTES
#include "fuse_app_config.h"
#endif
```

Initialize `fuse_hal_t` using the **grouped struct syntax** — only fill groups that exist on
this platform (matching `hal_groups` in `app_config.json`):
```c
fuse_hal_t hal;
memset(&hal, 0, sizeof(hal));
#ifdef FUSE_HAL_ENABLE_TIMER
hal.timer.get_timestamp = my_timer_fn;
#endif
#ifdef FUSE_HAL_ENABLE_CAMERA
hal.camera.last_frame   = my_camera_fn;
#endif
hal.quota_arm    = my_quota_arm_fn;
hal.quota_cancel = my_quota_cancel_fn;
```

Use `FUSE_POLICY_<MODULE>_*` macros from `fuse_app_config.h` to populate `fuse_policy_t`
instead of hardcoded numbers.

---

## Step 4 — Write the Demo CMakeLists.txt and build.sh (use `@developer`)

**Copy the boilerplate from `demos/demo_template/`** — both files are ready to use:

```bash
cp demos/demo_template/CMakeLists.txt demos/<name>/CMakeLists.txt
cp demos/demo_template/build.sh       demos/<name>/build.sh
chmod +x demos/<name>/build.sh
```

Then in `demos/<name>/CMakeLists.txt`, replace the one placeholder:
```cmake
project(<demo_name>_demo C CXX)   # ← replace <demo_name> with your demo name
```

Everything else is driven by the project name automatically (`DEMO_NAME`, `MODULE_NAME`,
output paths, target names). `build.sh` is generic — copy verbatim, no edits needed.

**Key design rules (already implemented in the template):**

- Each demo is a **standalone CMake project** — it is NOT added to the fuse root via
  `add_subdirectory`. It has its own `project()` declaration.
- The demo includes fuse as a subdirectory: `add_subdirectory("${FUSE_DIR}" fuse_build)`.
  Fuse detects it is not the top-level project and skips GoogleTest + tests.
- `FUSE_APP_CONFIG` is set to the demo's own `app_config.json` before `add_subdirectory`,
  so fuse compiles with only this demo's HAL groups.
- No explicit `target_include_directories` for fuse headers or generated config — they
  propagate automatically via the `fuse` target's PUBLIC properties:
  - `fuse/include` → `fuse.h`, `fuse_types.h`
  - `<build>/generated/` → `fuse_app_config.h`, `FUSE_POLICY_*` macros
  - `FUSE_HAL_ENABLE_*` compile definitions → consistent `fuse_hal_t` layout
- `_GNU_SOURCE` defined on the host executable for POSIX APIs (`clock_gettime`,
  `sigaction`, `usleep`) — Linux demo hosts require this with `-std=c99`.

**Do NOT add the demo to the fuse root `CMakeLists.txt`** — demos are fully standalone.

---

## Step 5 — Security Audit (use `@sentinel`)

All new C files must pass `@sentinel` before merging:
- Module source (`module/<name>.c`)
- Host source (`host/main.c`)

---

## Step 6 — Build and Test

```bash
# Build the demo standalone (drives fuse core compilation with its own HAL config)
cd demos/<name>
./build.sh

# Clean rebuild
./build.sh --clean

# Debug build
./build.sh --debug

# Run FUSE unit tests separately (fuse core build, all HAL groups enabled)
cd /path/to/fuse && ./build.py -c && cd build && ctest

# Run the demo
./build/<name>_host ./build/out/<module_name>.aot
```

---

## Review Checklist

Before a demo is merged, a reviewer should be able to answer all of the following by reading
only `app_config.json` (without reading any C source):

- [ ] What hardware peripherals does this deployment use?
- [ ] How much WAMR memory pool is allocated?
- [ ] How many modules will run simultaneously?
- [ ] For each module: what capabilities are granted?
- [ ] For each module: what is its memory budget (pages × 64 KiB + heap)?
- [ ] For each module: what is its maximum CPU time per step?
- [ ] For each module: what is the minimum time between steps?
