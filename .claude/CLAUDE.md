# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Session Startup Check
**At the start of every session, before doing any other work**, run this check:
```bash
test -f wasm-micro-runtime/wamr-compiler/build/wamrc && echo "wamrc OK" || echo "wamrc MISSING"
```
- If **MISSING**: immediately notify the user with this message before proceeding:
  > **wamrc is not built.** Unit tests that execute WASM modules will be skipped, and demo AOT binaries cannot be compiled. To build it (one-time, ~45 min):
  > ```bash
  > cd wasm-micro-runtime/wamr-compiler && ./build_llvm.sh --arch X86
  > mkdir -p build && cd build && cmake .. && make -j$(nproc)
  > ```
- If **OK**: proceed silently — do not mention wamrc to the user.

# FUSE runtime (Flexible Universal Secure Edge Runtime)
**NOTE**: Always update `.claude/CLAUDE.md`, never create a root `CLAUDE.md`
**Goals:** Space grade WAMR(Wasm-micro-runtime) based runtime that provide secure sandboxes for application modules, with review-friendly policies

## Terminology
**Terminologies defined here apply to all claude configuration markdown files**
- *WAMR* (Wasm-micro-runtime): backbone library that *FUSE* will be built upon, with WAMR's AOT mode.
- *Host*: hosting environment that *FUSE* will run in. for example satellite operator's RTOS, or baremetal. In UnitTest, it'll be x86 linux.
- *FUSE*: our core library that provides secure sandboxes for *Module*, run as a host process
- *Module*: applications that are compiled into wasm binary, which *FUSE* can load and execute under constraints defined in *Policy*.
- *Policy*: defines security, memory and system access bounds of a *Module*. It's expressed as a C structure in *FUSE*. After loading a *Module*, *FUSE* guarantees *Module* operates within bounds defined by *Policy*.

## Mission Context
A WAMR(wasm-micro-runtime) based, highly secure and flexible edge runtime library
- WAMR configuration: Enable AOT and integrate FUSE in AOT mode. disable JIT, disable interpretor, disable libc.
- Language: C, with google c++ style guide as the coding style standard wherever applicable to C.
- Build system: cmake
- Unit Testing framework: GoogleTest, with gMock for *Host* side hardware access implementations in testing
- Support *Policy* of each *Module* as a review-friendly json format.
- Claude code will be running inside a docker container, which is spawn from the image built from docker/Dockerfile.

## Critical Docs
- Technical Architecture: @architecture.md
- FUSE to Host API definition: @host_api_spec.md
- FUSE to Module API definition: @module_api_spec.md

## Agent Swarm Rules
- Use `@developer` for all C code implementation.
- Use `@sentinel` for all mandatory security audits of all C code.
- Use `@validator` to generate test cases for all C coding blocks.
- Use `@scripter` to generate any helper scripts, such as policy json to binary convertion.
- Follow the workflow defined in `@feature_addition.md` for how and when each agent should act when adding new functions for *FUSE* library.
- Follow the workflow defined in `@application_demo.md` for how and when each agent should act when adding new application demos.

## Project Structure
- `./core`: contains all .c source code for all core runtime functions and WAMR bridges
  - `./core/temp/`: temperature sensor HAL group (`fuse_hal_temp.h`, `fuse_hal_temp.c`)
  - `./core/timer/`: timer HAL group (`fuse_hal_timer.h`, `fuse_hal_timer.c`)
  - `./core/camera/`: camera HAL group (`fuse_hal_camera.h`, `fuse_hal_camera.c`)
  - `./core/log/`: log bridge — always compiled (`fuse_hal_log.h`, `fuse_hal_log.c`)
- `./include`: contains .h header files & api definitions
- `./tests`: contains all test cases
- `./demos/`: contains demo applications; each demo has its own `app_config.json`
- `./tools/`: contains scripts that @scripter may use and keep
  - `./tools/policy_to_bin.py`: standalone policy JSON → 24-byte binary converter
  - `./tools/gen_app_config.py`: app_config.json → C header + CMake flags + policy binaries
- `./wasm-micro-runtime/`: submodule that links to WAMR git repo, as project backbone
- `./CMakeLists.txt`: main cmake build entry
- `./build.py`: main build command entry to initiate compile and testings

## Standard Operating Procedures
- **Adding new features or implemeting APIs**: Follow the `@feature_addition.md` for the workflow
- **Adding new application demos**: Follow the `@application_demo.md` for the workflow
- **Memory Policy**: No dynamic allocation
- **Audit Requirement**: All code must receive a PASS from `@sentinel`

## Build & Test Commands
```bash
# Clean build — all HAL groups enabled by default (suitable for tests)
./build.py -c

# Build a demo standalone (each demo drives its own fuse core compilation)
cd demos/camera_compress && ./build.sh          # Release
cd demos/camera_compress && ./build.sh --clean  # clean rebuild
cd demos/camera_compress && ./build.sh --debug  # Debug

# Debug build
./build.py -c -b Debug

# Run all tests (after build)
cd build && ctest

# Run a single test by name
cd build && ./tests/fuse_test --gtest_filter=TestSuiteName.TestCaseName

# Generate app config artifacts manually (header + cmake flags + policy binaries)
python3 tools/gen_app_config.py \
    --input demos/camera_compress/app_config.json \
    --output-dir build/generated

# Compile a WASM module: C source → wasm → AOT
/opt/wasi-sdk/bin/clang --sysroot=/opt/wasi-sdk/share/wasi-sysroot -o module.wasm module.c
wamrc -o module.aot module.wasm
```
GoogleTest fetches automatically via CMake's `FetchContent` on first build. The test binary is `build/tests/fuse_test`.

**Note:** `wamrc` is not pre-installed. Build it once from the WAMR submodule (requires building LLVM first, ~45 min):
```bash
# Step 1: build LLVM (one-time, ~45 min)
cd wasm-micro-runtime/wamr-compiler && ./build_llvm.sh --arch X86

# Step 2: build wamrc
mkdir -p build && cd build && cmake .. && make -j$(nproc)
# Binary: wasm-micro-runtime/wamr-compiler/build/wamrc
# CMake find_program already searches this path automatically.
```
After wamrc is built, `./build.py -c` will auto-compile all .wat test modules to .aot.

## Docker Toolchain (paths inside container)
- **WASI SDK** (compile C→wasm): `/opt/wasi-sdk` — compiler at `/opt/wasi-sdk/bin/clang`, sysroot at `/opt/wasi-sdk/share/wasi-sysroot`
- **WABT** (wasm inspect/validate): `/opt/wabt/bin/` — includes `wasm2wat`, `wat2wasm`, `wasm-validate`
- **wamrc** (wasm→AOT): built from `wasm-micro-runtime/wamr-compiler/build/wamrc` (not pre-installed)

## Module Contract
Every FUSE *Module* **must** export `module_step()` as its primary entry point:
```c
__attribute__((export_name("module_step")))
void module_step(void) { /* one unit of work — no infinite loops */ }
```
Optional exports: `module_init()` (called once on first start), `module_deinit()` (called on unload).
`fuse_module_load()` will fail if `module_step` is not exported.

## Reviewable Application Policy
Every FUSE deployment **must** have an `app_config.json` at the demo root for review before deployment.
This single file declares: which hardware groups are available on the platform, memory pool sizes, and
each module's full policy (capabilities, memory, CPU quota, scheduling interval).

## HAL Group System
FUSE hardware APIs are organized into compile-time-conditional groups under `core/<group>/`:

| Group | Flag | Capability bit | Source | Note |
|-------|------|---------------|--------|------|
| `temp_sensor` | `FUSE_HAL_ENABLE_TEMP_SENSOR` | `FUSE_CAP_TEMP_SENSOR=0x01` | `core/temp/` | |
| `timer` | `FUSE_HAL_ENABLE_TIMER` | `FUSE_CAP_TIMER=0x02` | `core/timer/` | also drives log timestamps |
| `camera` | `FUSE_HAL_ENABLE_CAMERA` | `FUSE_CAP_CAMERA=0x04` | `core/camera/` | |
| `log` | *(always on)* | `FUSE_CAP_LOG=0x08` | `core/log/` | writes to FUSE internal ring buffer, no host callback |

**Key rules:**
- `hal_groups` in `app_config.json` lists hardware present on the platform (never include `"log"` — it's always registered)
- Module `capabilities` in policy must be a subset of `hal_groups ∪ {LOG}`
- `FUSE_HAL_ENABLE_*` flags are set PUBLIC on the `fuse` CMake target — consumers automatically inherit consistent struct layout
- Without `-DFUSE_APP_CONFIG=...`, all groups are enabled (default for test/dev builds)

## Application Config JSON (`app_config.json`)
Each demo defines its complete deployment in one reviewable file:
```json
{
  "application": {
    "name": "camera_compress",
    "max_modules": 1,
    "wamr_pool_bytes": 8388608,
    "log_pool_bytes": 65536,
    "hal_groups": ["timer", "camera"]
  },
  "modules": [
    {
      "name": "camera_compress",
      "binary": "out/camera_compress.aot",
      "policy": {
        "capabilities": ["TIMER", "CAMERA", "LOG"],
        "memory_pages_max": 62,
        "stack_size": 8192,
        "heap_size": 262144,
        "cpu_quota_us": 1000,
        "step_interval_us": 10000000
      }
    }
  ]
}
```
`tools/gen_app_config.py` validates and converts this to `fuse_app_config.h`, `fuse_hal_flags.cmake`, and per-module `*_policy.bin`.

## Key Types Quick Reference
*(Defined in `include/fuse_types.h` and `include/fuse.h` — no need to re-read those files)*

**`fuse_stat_t`** (return value of all public APIs):
`SUCCESS=0` `ERR_INVALID_ARG=1` `ERR_NOT_INITIALIZED=2` `ERR_ALREADY_INITIALIZED=3` `ERR_MODULE_LOAD_FAILED=4` `ERR_MODULE_NOT_FOUND=5` `ERR_MODULE_LIMIT=6` `ERR_POLICY_VIOLATION=7` `ERR_BUFFER_TOO_SMALL=8` `ERR_QUOTA_EXCEEDED=9` `ERR_MODULE_TRAP=10` `ERR_INTERVAL_NOT_ELAPSED=11`

**`fuse_module_state_t`**: `LOADED=0` `RUNNING=1` `PAUSED=2` `TRAPPED=3` `QUOTA_EXCEEDED=4` `UNLOADED=5`

**Capability bits**: `FUSE_CAP_TEMP_SENSOR=0x01` `FUSE_CAP_TIMER=0x02` `FUSE_CAP_CAMERA=0x04` `FUSE_CAP_LOG=0x08`

**`fuse_policy_t`** — 6 × uint32_t (24 bytes, little-endian binary layout):
```c
typedef struct { uint32_t capabilities; uint32_t memory_pages_max;
                 uint32_t stack_size; uint32_t heap_size; uint32_t cpu_quota_us;
                 uint32_t step_interval_us; /* min µs between steps; 0=no constraint */ } fuse_policy_t;
```
**Constants**: `FUSE_INVALID_MODULE_ID=UINT32_MAX` `FUSE_MAX_MODULES=8` `FUSE_LOG_MSG_MAX=128`

**`fuse_hal_t`** — compile-time conditional grouped struct (members present only for enabled groups):
```c
typedef struct {
    fuse_hal_temp_group_t    temp;         /* #ifdef FUSE_HAL_ENABLE_TEMP_SENSOR */
    fuse_hal_timer_group_t   timer;        /* #ifdef FUSE_HAL_ENABLE_TIMER       */
    fuse_hal_camera_group_t  camera;       /* #ifdef FUSE_HAL_ENABLE_CAMERA      */
    fuse_hal_quota_arm_fn    quota_arm;    /* always present; may be NULL        */
    fuse_hal_quota_cancel_fn quota_cancel; /* always present; may be NULL        */
} fuse_hal_t;
/* Group structs: .temp.get_reading  .timer.get_timestamp  .camera.last_frame */
```
Initialize with designated initializers: `hal.timer.get_timestamp = my_fn;`

**Complete `fuse.h` API signatures** (do not re-read fuse.h):
```c
fuse_stat_t fuse_init(void *mod_mem, size_t mod_mem_sz, void *log_mem, size_t log_mem_sz, const fuse_hal_t *hal);
fuse_stat_t fuse_stop(void);
fuse_stat_t fuse_restart(void);
fuse_stat_t fuse_module_load(const uint8_t *buf, uint32_t size, const fuse_policy_t *policy, fuse_module_id_t *out_id);
fuse_stat_t fuse_module_start(fuse_module_id_t id);
fuse_stat_t fuse_module_pause(fuse_module_id_t id);
fuse_stat_t fuse_module_stat(fuse_module_id_t id, fuse_module_state_t *out_state);
fuse_stat_t fuse_module_unload(fuse_module_id_t id);
fuse_stat_t fuse_module_run_step(fuse_module_id_t id);
void        fuse_quota_expired(fuse_module_id_t id);  /* ISR-safe */
uint32_t    fuse_tick(void);  /* run all due RUNNING modules; returns bitmask of IDs stepped */
fuse_stat_t fuse_policy_from_bin(const uint8_t *buf, uint32_t len, fuse_policy_t *out_policy);  /* deserialise 24-byte policy binary */
```

## Key WAMR/AOT Constraints (Lessons Learned)
- **No instruction metering in AOT mode.** `wasm_runtime_set_instruction_count_limit()` only works in interpreter mode. CPU quota in FUSE is time-based (host timer → `fuse_quota_expired()`).
- **WAMR AOT memory includes heap.** A module with `memory_pages_max=1` and `heap_size=8192` has a combined allocation of `65536+8192=73728` bytes. `wasm_runtime_validate_native_addr` validates against this combined size, not just linear memory. OOB tests must use lengths that clearly exceed the combined allocation.
- **`wasm_runtime_destroy()` must be guarded.** Calling it when WAMR was never initialized (or already destroyed) causes an abort. Always check `g_ctx.initialized` before calling it.
- **WAMR NativeSymbol `*~` signature** auto-converts module addresses to native pointers but does NOT guarantee bounds checking in AOT mode. Always call `wasm_runtime_validate_native_addr()` explicitly for buffer arguments.
