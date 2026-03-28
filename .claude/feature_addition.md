# Feature Addition Workflow

This document defines the step-by-step process for adding new features or APIs to the FUSE library.
It covers two scenarios: adding a new HAL hardware group and adding a new core FUSE API.

---

## Scenario A — Adding a New HAL Hardware Group

A HAL group is a compile-time-optional set of native functions that modules can call to access
a hardware peripheral. Each group lives under `core/<group>/`.

### Step 1 — Design the Group API (use `@developer`)

Decide:
- **Group name**: lowercase, e.g. `gps`
- **Capability bit**: next available bit in `fuse_types.h` (e.g. `FUSE_CAP_GPS=0x10`)
- **Enable flag**: `FUSE_HAL_ENABLE_GPS`
- **Group sub-struct fields**: one function pointer per native function exposed to modules
- **Module-callable functions**: what WASM modules can import from `env`

### Step 2 — Create Group Header `core/<group>/fuse_hal_<group>.h` (use `@developer`)

Pattern (copy from existing group, e.g. `core/timer/fuse_hal_timer.h`):

```c
#ifndef FUSE_HAL_GPS_H
#define FUSE_HAL_GPS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Host-supplied callback(s): */
    int (*get_fix)(double *lat_out, double *lon_out);
} fuse_hal_gps_group_t;

/* Called once from fuse_init() after wasm_runtime_full_init() */
void fuse_hal_gps_register_natives(void);

#ifdef __cplusplus
}
#endif
#endif /* FUSE_HAL_GPS_H */
```

Rules:
- No WAMR headers in the group header (keep it host-facing and dependency-free)
- One `fuse_hal_<group>_register_natives()` declaration only

### Step 3 — Create Group Source `core/<group>/fuse_hal_<group>.c` (use `@developer`)

Pattern:

```c
#ifdef FUSE_HAL_ENABLE_GPS

#include "fuse_hal_gps.h"
#include "../fuse_internal.h"
#include "wasm_export.h"

static void fuse_native_gps_get_fix(
    wasm_exec_env_t exec_env,
    double *lat_out, double *lon_out)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    fuse_module_desc_t *desc = fuse_get_module_desc_by_inst(inst);
    if (!desc) return;
    if (!(desc->policy.capabilities & FUSE_CAP_GPS)) {
        fuse_policy_violation(desc, inst, "GPS");
        return;
    }
    /* validate output pointer arguments here if needed */
    if (g_ctx.hal.gps.get_fix != NULL) {
        g_ctx.hal.gps.get_fix(lat_out, lon_out);
    }
}

static NativeSymbol s_gps_symbols[] = {
    { "gps_get_fix", fuse_native_gps_get_fix, "(**)" }
};

void fuse_hal_gps_register_natives(void) {
    wasm_runtime_register_natives(
        "env", s_gps_symbols,
        sizeof(s_gps_symbols) / sizeof(s_gps_symbols[0]));
}

#endif /* FUSE_HAL_ENABLE_GPS */
```

Rules:
- Entire file body wrapped in `#ifdef FUSE_HAL_ENABLE_<GROUP>` / `#endif`
- Always check capability bit, call `fuse_policy_violation()` on failure
- Call `wasm_runtime_validate_native_addr()` for any pointer/buffer arguments
- `NativeSymbol` function signature strings: see WAMR docs (`*` = pointer, `~` = auto-convert, `i`/`I`/`f`/`F` = 32/64-bit int/float)

### Step 4 — Update `include/fuse.h` (use `@developer`)

Add the group header include and struct member:

```c
/* In the group-includes block near the top: */
#ifdef FUSE_HAL_ENABLE_GPS
#  include "../core/gps/fuse_hal_gps.h"
#endif

/* In fuse_hal_t struct: */
#ifdef FUSE_HAL_ENABLE_GPS
    fuse_hal_gps_group_t  gps;   /* FUSE_CAP_GPS */
#endif
```

### Step 5 — Update `fuse_types.h` (use `@developer`)

Add the new capability bit constant:
```c
#define FUSE_CAP_GPS   0x10u
```

### Step 6 — Wire into `core/fuse_core.c` (use `@developer`)

Add group registration after `wasm_runtime_full_init()`:
```c
#ifdef FUSE_HAL_ENABLE_GPS
    fuse_hal_gps_register_natives();
#endif
```

Add group header include in `fuse_internal.h` (or directly in `fuse_core.c` if preferred):
```c
#ifdef FUSE_HAL_ENABLE_GPS
#  include "gps/fuse_hal_gps.h"
#endif
```

### Step 7 — Update `tools/gen_app_config.py` (use `@scripter`)

Add the new group to:
1. `VALID_HAL_GROUPS` set
2. `HAL_GROUP_TO_CAP` mapping
3. `HAL_GROUP_TO_FLAG` mapping (generates `FUSE_HAL_ENABLE_GPS=1`)

### Step 8 — Update `CMakeLists.txt`

Add group source dir to the SRCS glob:
```cmake
"${CMAKE_CURRENT_SOURCE_DIR}/core/gps/*.c"
```

### Step 9 — Update Documentation

- `.claude/CLAUDE.md`: add row to HAL Group System table
- `.claude/architecture.md`: add row to HAL Group Architecture table
- `.claude/host_api_spec.md`: add group sub-struct and usage example
- `.claude/module_api_spec.md`: add new HAL API function entry

### Step 10 — Security Audit (use `@sentinel`)

All new `.c` files must pass `@sentinel` before merging.

### Step 11 — Write Tests (use `@validator`)

Add test cases covering:
- Module with capability granted: native function called successfully
- Module without capability: `FUSE_ERR_POLICY_VIOLATION` + security log entry
- NULL host callback: no crash, returns gracefully
- Buffer out-of-bounds (if group has pointer args): module trapped

### Step 12 — Build and Verify

```bash
./build.py -c
cd build && ctest
```

---

## Scenario B — Adding a New Core FUSE API

Core APIs live in `include/fuse.h` (declaration) and `core/fuse_<area>.c` (implementation).

### Step 1 — Design the API

Specify:
- Function signature following `fuse_stat_t fuse_<verb>_<noun>(...)` convention
- Which return codes apply (`fuse_stat_t` values)
- Pre-conditions (e.g. must be initialized, module must be in a specific state)
- Side effects on module state machine

### Step 2 — Declare in `include/fuse.h` (use `@developer`)

Add the function signature with a one-line doc comment.

### Step 3 — Implement in `core/fuse_<area>.c` (use `@developer`)

Rules:
- Check `g_ctx.initialized` first → `FUSE_ERR_NOT_INITIALIZED`
- Validate all pointer arguments → `FUSE_ERR_INVALID_ARG`
- No dynamic allocation (no `malloc`/`free`)
- Update module state atomically if modifying state machine

### Step 4 — Update `.claude/host_api_spec.md`

Add the new API with its full signature, description, pre-conditions, and return values.

### Step 5 — Security Audit (use `@sentinel`)

Run `@sentinel` on the modified `.c` file.

### Step 6 — Write Tests (use `@validator`)

Test:
- Happy path
- Not-initialized precondition
- Invalid argument cases
- State machine transitions

### Step 7 — Build and Verify

```bash
./build.py -c
cd build && ctest
```
