# FUSE — Flexible Universal Secure Edge Runtime

![FUSE Logo](fuse-logo.png)

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

FUSE is a deterministic edge FaaS runtime built for resource-constrained and safety-critical systems, with space and satellite applications as a primary target.
Built on [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime), it provides isolated, policy-bounded sandboxes with predictable scheduling and resource consumption — suitable for RTOS environments, bare-metal systems, and on-board computers.

> **Early-stage Project** — APIs and interfaces are evolving rapidly. Frequent breaking changes should be expected. Feedback and contributions of any kind are very welcome.
>
> **Extra Reading** — Read [FUSE Runtime](https://xinch.substack.com/p/fuse-runtime-for-edge-faas) about the motivations, goals, architectures, and agent swarm workflows in detail.

## Key Properties

- **Review-friendly policy** — every deployment is described by a single `app_config.json` that captures hardware capabilities, memory budgets, CPU quotas, and scheduling parameters in human-readable form
- **Hardware portability** — FUSE is easily portable across different host (RTOS, bare-metal, Linux); only a thin HAL callback layer needs porting to new hardware
- **Language portability** — modules can be written in any language that compiles to WASM (C, C++, Rust, Zig, and others); the FUSE runtime itself is pure C
- **Strict isolation** — each module runs in its own WASM linear memory; cross-module and host memory access is impossible by construction
- **Fail-safe** — module failures are guaranteed to be trapped within the sandbox and do not affect the host
- **AOT execution** — modules are compiled to native code ahead of time; no interpreter overhead and near native execution
- **Flexible scheduling** — modules may be activated by explicit host trigger, time based trigger, or event trigger. Including hardware events, or event chaining between modules enabling zero-polling pipeline topologies
- **Flexible runtime adjustment** — modules can be added, removed, and updated at runtime

## Architecture

```
┌─────────────────────────────────────────────────────┐
│  Host (RTOS / bare-metal)                           │
│  ┌───────────────────────────────────────────────┐  │
│  │  FUSE Runtime                                 │  │
│  │  ┌─────────────┐        ┌─────────────┐       │  │
│  │  │  Module A   │        │  Module B   │       │  │
│  │  │  (WASM/AOT) │        │  (WASM/AOT) │       │  │
│  │  └──────┬──────┘        └──────┬──────┘       │  │
│  │         │  policy-checked API  │              │  │
│  │  ┌──────▼──────────────────────▼────────────┐ │  │
│  │  │  HAL Bridge  │  Log  │  Event Bus        │ │  │
│  │  └──────────────────────────────────────────┘ │  │
│  └───────────────────────────────────────────────┘  │
│  Hardware: timer, camera, temp sensor, …            │
└─────────────────────────────────────────────────────┘
```

Modules communicate with the outside world exclusively through FUSE's policy-checked native imports. Direct host memory access, cross-module calls, and unchecked hardware access are structurally impossible.

## Policy Model

Every module is governed by a policy that governs what a module is allowed to do. Full security posture of a deployment can be determined by reading policy alone.
```json
"policy": {
    "capabilities": ["TIMER", "CAMERA", "LOG"],
    "memory_pages_max": 62,
    "stack_size": 8192,
    "heap_size": 262144,
    "cpu_quota_us": 1000,
    "step_interval_us": 10000000,
    "activation_mask": ["INTERVAL"],
    "event_subscribe": 0
}
```

| Field | Description |
|---|---|
| `capabilities` | Which HAL groups the module may access (timer, camera, temp sensor, event, etc) |
| `memory_pages_max` | Maximum WASM linear memory (1 page = 64 KiB) |
| `stack_size` / `heap_size` | WASM stack and heap budgets in bytes |
| `cpu_quota_us` | Max microseconds per step; 0 = unlimited |
| `step_interval_us` | Minimum interval between steps for interval-driven modules |
| `activation_mask` | Activation modes: interval, event, or manual |
| `event_subscribe` | Bitmask of event IDs (0–31) this module reacts to |

Policies are declared in `app_config.json` alongside the application configuration, converted to binary by `tools/gen_app_config.py`, and can also be loaded dynamically at runtime.

## HAL Groups

Hardware APIs are organised into compile-time-conditional groups. Only groups present on the target platform are compiled in, producing zero dead code for absent hardware.

| Group | Capability | Always present |
|---|---|---|
| Temperature sensor | `TEMP_SENSOR` | No |
| Timer | `TIMER` | No |
| Camera | `CAMERA` | No |
| Log | `LOG` | Yes |
| Event post | `EVENT_POST` | Yes |

Groups are selected via `hal_groups` in `app_config.json`. Without an app config (dev/test builds), all groups are enabled.

## Getting Started

### Prerequisites

- `wasi-sdk` (C/C++ → WASM)
- `wabt` (WASM inspection)
- `wamrc` (the WAMR AOT compiler) must be built from the WAMR submodule.

### Build & Test

```bash
# Clean build FUSE Runtime library (all HAL groups enabled — suitable for tests)
./build.py -c

# Run all tests
cd build && ctest
```

### Build a Demo

Each demo is a standalone project with its own build script and `app_config.json`:

```bash
cd demos/camera_compress && ./build.sh          # release
cd demos/camera_compress && ./build.sh --clean  # clean rebuild
```

## Module Development

### Execution Model

FUSE drives modules by calling a single exported entry point — `module_step()` — once per scheduling quantum. Modules must complete all work within that one call and return. Infinite loops inside `module_step()` will exhaust the CPU quota and cause the module to be trapped. Two optional lifecycle exports, `module_init()` and `module_deinit()`, are called once on first start and on unload respectively.

### Execution Invocation

There are three ways module step function can be triggered. They can be combined:

- *MANUAL*: Host has full control when to invoke. Most predicatable and well-suited for tightly coordinated pipelines.
- *INTERVAL*: Eligible to run when the `step_interval_us` has elapsed since last invocation.
- *EVENT*: Either a host generated event, or by another module. Enables event-chaining pipelines where upstream module signal downstream module without polling.

### Requirements

- **No standard library** — modules are freestanding WASM binaries with no libc and no WASI. Standard I/O functions such as `printf` are unavailable. All output goes through the FUSE log API.
- **No infinite loops** — `module_step()` must return promptly. FUSE enforces a per-module CPU quota; exceeding it terminates and traps the module.
- **No dynamic memory** — modules must not rely on `malloc`/`free`. Static and stack allocation only. WASM linear memory is zero-initialised at startup.
- **Declare all imports explicitly** — every FUSE API a module uses must be declared as a WASM import with the `env` module name and the correct function name. Undeclared imports will cause load failure.
- **Export required symbols** — `module_step` must be exported. Omitting it will cause `fuse_module_load()` to fail.
- **Stay within policy bounds** — any attempt to access a hardware API not listed in the module's `capabilities`, or to post an out-of-range event ID, results in an immediate policy violation trap.

### Compilation Pipeline

Modules are compiled in two stages: source language → WASM binary, then WASM binary → AOT native binary via `wamrc`. The resulting `.aot` file is what FUSE loads at runtime. The WASM intermediate is discarded after AOT compilation.

## Demos

### `camera_compress`

A single-module demo that captures camera frames and compresses them using run-length encoding (RLE). Demonstrates interval-based scheduling, camera and timer HAL access, and policy enforcement.

### `camera_filter_compress`

A two-module event-chained pipeline. A `frame_filter` module wakes on a `CAMERA_FRAME_READY` event, analyses each frame's variance, and forwards only qualifying frames by posting a `DATA_CAPTURED` event. A `frame_compress` module wakes on `DATA_CAPTURED` and applies RLE compression. Neither module polls — each wakes only when its subscribed event fires.

```
[ISR] CAMERA_FRAME_READY
        │
        ▼
  frame_filter  ── variance check ──►  DATA_CAPTURED
                                            │
                                            ▼
                                     frame_compress  ── RLE output
```

## Supported Platforms

FUSE separates platform-specific HAL code (timer, quota interrupt) from the OS-agnostic core. Implementations live under `platform/<name>/` and expose a common interface (`platform/platform.h`). Select a platform at build time with `-DFUSE_PLATFORM=<name>` (default: `linux`).

| Platform | Architecture | HAL implementation | CI tested |
|---|---|---|---|
| `linux` | x86-64 | `platform/linux/` — `clock_gettime`, `SIGALRM`/`setitimer` | Yes |
| `freertos` | ARM Cortex-M4 (POSIX sim for CI) | `platform/freertos/` — `xTaskGetTickCount`, `xTimerCreate` | Yes (POSIX simulator) |

**Adding a new platform** requires implementing the four-function `platform/platform.h` interface (`init`, `get_timestamp_us`, `quota_arm`, `quota_cancel`) and a small CMakeLists.txt. No changes to the FUSE core are needed.

> **FreeRTOS note:** software timer resolution is limited by `configTICK_RATE_HZ` (1 ms at 1 kHz). CPU quotas shorter than one tick round up to one tick. For sub-millisecond quota precision on real hardware, wire a hardware timer peripheral directly to `quota_arm`/`quota_cancel`.

## Extending Hardware Support

FUSE's HAL group system is designed to be extended. Adding support for a new hardware peripheral means implementing a new group under `core/<group>/` — a header defining the callback struct, a source file registering the native symbols with WAMR, and a corresponding capability bit and compile flag. The rest of the runtime requires no changes.

Community contributions for new HAL groups are welcome. If you are integrating FUSE on a new platform and add support for hardware not yet covered (e.g. UART, SPI, GPIO, GNSS), please consider opening a pull request so others can benefit.

## Co-developed with Claude Code Agent Swarm

FUSE is co-developed with [Claude Code](https://claude.ai/code). The project uses a multi-agent swarm setup where specialized Claude agents handle distinct roles in the development workflow:

| Agent | Role |
|---|---|
| `@developer` | C code implementation — core runtime, HAL bridges, platform layers |
| `@sentinel` | Security audit — mandatory review of all C code before merge |
| `@validator` | Test generation — unit and integration test cases for every coding block |
| `@scripter` | Tooling — helper scripts such as policy JSON to binary conversion |

The agent workflows are defined in `.claude/feature_addition.md` and `.claude/application_demo.md`, covering when and how each agent is invoked for new features and demos. All agents operate under the same coding standards and security requirements as human contributors.

Contributions to the Claude Code setup itself are welcome — if you have improvements to the agent workflows, prompts, or tooling scripts, feel free to open a pull request.

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details.

FUSE builds on [wasm-micro-runtime](https://github.com/bytecodealliance/wasm-micro-runtime) (Apache 2.0).
