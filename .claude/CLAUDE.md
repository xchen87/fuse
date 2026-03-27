# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

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
- Follow the workflow defined in `@feature_addition.md` for how and when each agent should act when adding new code.

## Project Structure
- `./core`: contains all .c source code for all core runtime functions and WAMR bridges
- `./include`: contains .h header files & api definitions
- `./tests`: contains all test cases
- `./modules/`: contains all example module applications source code
- `./cmake/`: contains all .cmake files
- `./wasm-micro-runtime/`: submodule that links to WAMR git repo, as project backbone
- `./CMakeLists.txt`: main cmake build entry
- `./build.py`: main build command entry to initiate compile and testings

## Standard Operating Procedures
- **Adding new features or implemeting APIs**: Follow the `@feature_addition.md` for the workflow
- **Memory Policy**: No dynamic allocation
- **Audit Requirement**: All code must receive a PASS from `@sentinel`

## Build & Test Commands
```bash
# Clean build (recommended when changing CMake config)
./build.py -c

# Debug build
./build.py -c -b Debug

# Run all tests (after build)
cd build && ctest

# Run a single test by name
cd build && ./tests/fuse_test --gtest_filter=TestSuiteName.TestCaseName

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

## Key WAMR/AOT Constraints (Lessons Learned)
- **No instruction metering in AOT mode.** `wasm_runtime_set_instruction_count_limit()` only works in interpreter mode. CPU quota in FUSE is time-based (host timer → `fuse_quota_expired()`).
- **WAMR AOT memory includes heap.** A module with `memory_pages_max=1` and `heap_size=8192` has a combined allocation of `65536+8192=73728` bytes. `wasm_runtime_validate_native_addr` validates against this combined size, not just linear memory. OOB tests must use lengths that clearly exceed the combined allocation.
- **`wasm_runtime_destroy()` must be guarded.** Calling it when WAMR was never initialized (or already destroyed) causes an abort. Always check `g_ctx.initialized` before calling it.
- **WAMR NativeSymbol `*~` signature** auto-converts module addresses to native pointers but does NOT guarantee bounds checking in AOT mode. Always call `wasm_runtime_validate_native_addr()` explicitly for buffer arguments.
