# =============================================================================
# demos/camera_filter_compress — Event-chained filter + compress pipeline demo
#
# Standalone CMake project.  Builds fuse (with only the HAL groups this demo
# needs, as declared in app_config.json), compiles both WASM modules to AOT,
# and builds the host executable.
#
# Usage:
#   ./build.sh                     — configure + build (Release)
#   ./build.sh --clean             — clean, then configure + build
#   ./build.sh --debug             — Debug build
#   ./build.sh --fuse-dir <path>   — point to a fuse root outside this tree
#
#   Or drive cmake directly:
#   cmake -S . -B build [-DFUSE_DIR=<path>] [-DCMAKE_BUILD_TYPE=Release]
#   cmake --build build
# =============================================================================

cmake_minimum_required(VERSION 3.14)
project(camera_filter_compress_demo C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Locate the fuse repository root.
#
# Default: two levels up from this CMakeLists.txt (demos/<name>/ → repo root).
# Override with -DFUSE_DIR=<path> when building out-of-tree.
# ---------------------------------------------------------------------------
if(NOT DEFINED FUSE_DIR)
    get_filename_component(FUSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
if(NOT EXISTS "${FUSE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "[camera_filter_compress] FUSE_DIR does not contain a CMakeLists.txt: ${FUSE_DIR}\n"
        "  Pass -DFUSE_DIR=<path> to point to the fuse repository root.")
endif()

# ---------------------------------------------------------------------------
# App config — selects which HAL groups are compiled into fuse.
# Must be set before add_subdirectory so fuse's CMakeLists.txt picks it up.
# ---------------------------------------------------------------------------
set(FUSE_APP_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/app_config.json"
    CACHE PATH "Application configuration JSON for this demo")

# ---------------------------------------------------------------------------
# Build fuse with this demo's HAL configuration.
#
# fuse's CMakeLists.txt detects it is not the top-level project and skips
# GoogleTest + tests.  It runs gen_app_config.py, writes generated headers
# into <build>/generated/, and exposes the 'fuse' CMake target with PUBLIC
# include dirs (fuse/include, <build>/generated/) and PUBLIC compile
# definitions (FUSE_HAL_ENABLE_*) — all propagated to
# camera_filter_compress_host automatically via target_link_libraries below.
# ---------------------------------------------------------------------------
add_subdirectory("${FUSE_DIR}" fuse_build)

# ---------------------------------------------------------------------------
# Toolchain discovery
# ---------------------------------------------------------------------------
find_program(WASI_CLANG clang HINTS /opt/wasi-sdk/bin NO_DEFAULT_PATH)
find_program(WAMRC wamrc HINTS
    "${FUSE_DIR}/wasm-micro-runtime/wamr-compiler/build"
    /opt/wamrc
    /usr/local/bin
)

if(WASI_CLANG)
    message(STATUS "[demo:camera_filter_compress] wasi-clang : ${WASI_CLANG}")
else()
    message(WARNING "[demo:camera_filter_compress] wasi-clang not found — WASM modules will NOT be compiled")
endif()

if(WAMRC)
    message(STATUS "[demo:camera_filter_compress] wamrc      : ${WAMRC}")
else()
    message(WARNING "[demo:camera_filter_compress] wamrc not found — AOT binaries will NOT be generated")
endif()

# ---------------------------------------------------------------------------
# Output directory for generated binaries
# ---------------------------------------------------------------------------
set(DEMO_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/out")
file(MAKE_DIRECTORY "${DEMO_OUT_DIR}")

# ---------------------------------------------------------------------------
# WASM modules: C → .wasm → .aot
# ---------------------------------------------------------------------------
if(WASI_CLANG AND WAMRC)
    # --- frame_filter ---
    set(FILTER_WASM "${DEMO_OUT_DIR}/frame_filter.wasm")
    set(FILTER_AOT  "${DEMO_OUT_DIR}/frame_filter.aot")

    add_custom_command(
        OUTPUT  "${FILTER_WASM}"
        COMMAND "${WASI_CLANG}"
                --target=wasm32-unknown-unknown
                -nostdlib
                -O2
                -Wl,--no-entry
                -Wl,--export=module_step
                -Wl,--allow-undefined
                -o "${FILTER_WASM}"
                "${CMAKE_CURRENT_SOURCE_DIR}/module/frame_filter.c"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/module/frame_filter.c"
        COMMENT "[demo:camera_filter_compress] frame_filter.c → WASM"
    )

    add_custom_command(
        OUTPUT  "${FILTER_AOT}"
        COMMAND "${WAMRC}"
                --target=x86_64
                --target-abi=gnu
                -o "${FILTER_AOT}"
                "${FILTER_WASM}"
        DEPENDS "${FILTER_WASM}"
        COMMENT "[demo:camera_filter_compress] frame_filter WASM → AOT"
    )

    # --- frame_compress ---
    set(COMPRESS_WASM "${DEMO_OUT_DIR}/frame_compress.wasm")
    set(COMPRESS_AOT  "${DEMO_OUT_DIR}/frame_compress.aot")

    add_custom_command(
        OUTPUT  "${COMPRESS_WASM}"
        COMMAND "${WASI_CLANG}"
                --target=wasm32-unknown-unknown
                -nostdlib
                -O2
                -Wl,--no-entry
                -Wl,--export=module_step
                -Wl,--allow-undefined
                -o "${COMPRESS_WASM}"
                "${CMAKE_CURRENT_SOURCE_DIR}/module/frame_compress.c"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/module/frame_compress.c"
        COMMENT "[demo:camera_filter_compress] frame_compress.c → WASM"
    )

    add_custom_command(
        OUTPUT  "${COMPRESS_AOT}"
        COMMAND "${WAMRC}"
                --target=x86_64
                --target-abi=gnu
                -o "${COMPRESS_AOT}"
                "${COMPRESS_WASM}"
        DEPENDS "${COMPRESS_WASM}"
        COMMENT "[demo:camera_filter_compress] frame_compress WASM → AOT"
    )

    add_custom_target(camera_filter_compress_aot ALL
        DEPENDS "${FILTER_AOT}" "${COMPRESS_AOT}"
    )
    message(STATUS "[demo:camera_filter_compress] WASM toolchain found — modules will be compiled to AOT")
else()
    message(WARNING
        "[demo:camera_filter_compress] wasi-clang or wamrc missing — skipping WASM/AOT compilation.\n"
        "  To build wamrc:\n"
        "    cd ${FUSE_DIR}/wasm-micro-runtime/wamr-compiler && ./build_llvm.sh --arch X86\n"
        "    mkdir build && cd build && cmake .. && make -j\$(nproc)\n"
    )
endif()

# ---------------------------------------------------------------------------
# Host executable
#
# No explicit target_include_directories needed — everything propagates
# transitively from the 'fuse' PUBLIC target:
#   fuse/include            → fuse.h, fuse_types.h
#   <build>/generated/      → fuse_app_config.h (FUSE_APP_* / FUSE_POLICY_* / FUSE_EVENT_* macros)
#   FUSE_HAL_ENABLE_*       → consistent fuse_hal_t layout
# ---------------------------------------------------------------------------
add_executable(camera_filter_compress_host host/main.c)
target_link_libraries(camera_filter_compress_host PRIVATE fuse)
# _GNU_SOURCE enables POSIX + GNU extensions (clock_gettime, sigaction, usleep)
# on Linux.  Demo hosts are Linux-specific; this is intentional.
target_compile_definitions(camera_filter_compress_host PRIVATE _GNU_SOURCE)
target_compile_options(camera_filter_compress_host PRIVATE
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O2>
)

if(WASI_CLANG AND WAMRC)
    add_dependencies(camera_filter_compress_host camera_filter_compress_aot)
endif()
