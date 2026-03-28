# Skill: Implement a demo application with *FUSE*
### Description
Generate host control code and WASM modules, with policies written in JSON.
Each demo should be treated as a separate CMake project that links and uses FUSE. Each demo should meet the goal described by user.
Create dedicated folder under `/demos`.

### Required User Input
The user MUST provide a clear GOAL describing:
- The intended functionality of the system
- What the WASM module should do
- What the host application should control or enforce

### WASM Module Development Reference
*(avoids re-reading toolchain docs and existing CMakeLists)*

**Freestanding compilation** (no WASI imports — required because `WAMR_BUILD_LIBC_WASI=0`):
```bash
/opt/wasi-sdk/bin/clang \
  --target=wasm32-unknown-unknown -nostdlib -O2 \
  -Wl,--no-entry \
  -Wl,--export=module_step,--export=module_init \
  -Wl,--allow-undefined \
  -o module.wasm module.c
wamrc --target=x86_64 --target-abi=gnu -o module.aot module.wasm
```

**Module C source constraints** (no libc — no printf, sprintf, memset, memcpy):
```c
/* Declare imports: */
__attribute__((import_module("env"), import_name("camera_last_frame")))
extern uint64_t camera_last_frame(void *buf, unsigned int max_len);
/* Other imports: timer_get_timestamp, module_log_event, temp_get_reading */

/* Required export: */
__attribute__((export_name("module_step"))) void module_step(void) { /* no infinite loops */ }
/* Optional: module_init, module_deinit */
```

**CMake demo pattern** (subdirectory added via root `add_subdirectory(demos/<name>)`):
```cmake
find_program(WASI_CLANG clang HINTS /opt/wasi-sdk/bin NO_DEFAULT_PATH)
find_program(WAMRC wamrc HINTS ${CMAKE_SOURCE_DIR}/wasm-micro-runtime/wamr-compiler/build /opt/wamrc /usr/local/bin)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Policy JSON → binary (always runs):
add_custom_command(OUTPUT policy.bin
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/policy_to_bin.py"
            --input policy.json --output policy.bin
    DEPENDS policy.json "${CMAKE_SOURCE_DIR}/tools/policy_to_bin.py")
add_custom_target(my_policy ALL DEPENDS policy.bin)

# C → WASM → AOT (skipped gracefully if toolchain absent):
if(WASI_CLANG AND WAMRC)
    add_custom_command(OUTPUT module.wasm COMMAND ${WASI_CLANG} ... DEPENDS module.c)
    add_custom_command(OUTPUT module.aot  COMMAND ${WAMRC} ...    DEPENDS module.wasm)
    add_custom_target(my_aot ALL DEPENDS module.aot)
endif()

# Host executable uses parent project's 'fuse' target (no find_package needed):
add_executable(my_host host/main.c)
target_link_libraries(my_host PRIVATE fuse)
target_include_directories(my_host PRIVATE "${CMAKE_SOURCE_DIR}/include")
```

**Policy JSON template** (all fields required by `tools/policy_to_bin.py`, outputs 24-byte binary):
```json
{
    "capabilities": 14,
    "memory_pages_max": 62,
    "stack_size": 8192,
    "heap_size": 262144,
    "cpu_quota_us": 1000,
    "step_interval_us": 10000000
}
```
- `capabilities`: OR of `FUSE_CAP_CAMERA=0x04`, `FUSE_CAP_LOG=0x08`, `FUSE_CAP_TIMER=0x02`, `FUSE_CAP_TEMP_SENSOR=0x01`
- `step_interval_us=0` disables interval enforcement (host drives timing manually)
- `cpu_quota_us=0` disables the 1ms wall-clock quota

**Memory constraint** (4MB combined = `memory_pages_max=62` + `heap_size=262144`):
- 62 × 65536 + 262144 = 4,194,304 bytes exactly
- Static buffers in module C code count against linear memory (62 pages = 3,932,160 bytes available)

### Instructions
1. **Goal Clarification**: If the GOAL is missing or vague, ask the user to clarify before proceeding.
  - Do NOT generate code until a concrete GOAL is provided.
  - Once GOAL is clear, proceed to **Design** phase.
2. **Design**: Reference `@architecture.md`, `@host_api_spec.md`, and `@module_api_spec.md` for context and pre-defined API spec.
  - Define the necessary API signatures and the execution flow.
  - Review and refine until it can meet the GOAL. Then proceed to **Coding** phase
3. **Coding**: delegate to @developer to write the C implementations. Coding should include:
  - Generate host control code, including *FUSE* management, *Module* loading and calling sequence, post processing after each *Module* step. Teardown after everything is done, if the task does not require continuously running.
  - Generate WASM module(s), that implements the core functionalities required for the task.
  - Generate JSON policy, with fields matches the *Module* policy defintion. In build system, use json to binary convertion script for converting the policy to execution ready binary. If such script doesn't exist, ask @scripter to create one.
  - Structure as separate CMake projects that links FUSE library.
  - Once done, proceed to **Security Audit**.
4. **Security Audit**
   - Delegate to @sentinel to perform a security audit.
   - Output: Must receive a "PASS" verdict from @sentinel, once received, go to **Final** phase.
   - If received a "FAIL" verdict, notify @developer on the details of the failure and ask for a code update.
5. **Final**
   - Git commit for all code implementations, audit trails.
