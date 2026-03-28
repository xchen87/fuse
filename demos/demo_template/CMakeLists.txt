# =============================================================================
# demos/<demo_name> — <human description>
#
# Standalone CMake project.  Copy this directory to demos/<demo_name>/,
# replace every occurrence of <demo_name> with your demo's name, and fill
# in app_config.json.
#
# Usage:
#   ./build.sh                   — configure + build (Release)
#   ./build.sh --clean           — clean then build
#   ./build.sh --debug           — Debug build
# =============================================================================

cmake_minimum_required(VERSION 3.14)

# === REPLACE <demo_name> with your demo name (lowercase, valid C identifier) ===
project(<demo_name>_demo C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Derive demo and module names from the project name.
# Override MODULE_NAME below if the WASM module has a different name.
string(REPLACE "_demo" "" DEMO_NAME "${PROJECT_NAME}")
set(MODULE_NAME "${DEMO_NAME}")

# ---------------------------------------------------------------------------
# Locate the fuse repository root.
# Default: two levels up from demos/<demo_name>/.
# Override with -DFUSE_DIR=<path> when building out-of-tree.
# ---------------------------------------------------------------------------
if(NOT DEFINED FUSE_DIR)
    get_filename_component(FUSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
if(NOT EXISTS "${FUSE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "[${DEMO_NAME}] FUSE_DIR does not contain a CMakeLists.txt: ${FUSE_DIR}\n"
        "  Pass -DFUSE_DIR=<path> to the fuse repository root.")
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
# Skips GoogleTest + tests (fuse detects it is not the top-level project).
# Propagates PUBLIC to ${DEMO_NAME}_host via target_link_libraries:
#   • fuse/include            → fuse.h, fuse_types.h
#   • <build>/generated/      → fuse_app_config.h
#   • FUSE_HAL_ENABLE_*       → consistent fuse_hal_t layout
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
    message(STATUS "[demo:${DEMO_NAME}] wasi-clang : ${WASI_CLANG}")
else()
    message(WARNING "[demo:${DEMO_NAME}] wasi-clang not found — WASM module will NOT be compiled")
endif()

if(WAMRC)
    message(STATUS "[demo:${DEMO_NAME}] wamrc      : ${WAMRC}")
else()
    message(WARNING "[demo:${DEMO_NAME}] wamrc not found — AOT binary will NOT be generated")
endif()

# ---------------------------------------------------------------------------
# Output directory for compiled binaries
# ---------------------------------------------------------------------------
set(DEMO_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/out")
file(MAKE_DIRECTORY "${DEMO_OUT_DIR}")

# ---------------------------------------------------------------------------
# WASM module: C → .wasm → .aot
# ---------------------------------------------------------------------------
if(WASI_CLANG AND WAMRC)
    set(MODULE_WASM "${DEMO_OUT_DIR}/${MODULE_NAME}.wasm")
    set(MODULE_AOT  "${DEMO_OUT_DIR}/${MODULE_NAME}.aot")

    add_custom_command(
        OUTPUT  "${MODULE_WASM}"
        COMMAND "${WASI_CLANG}"
                --target=wasm32-unknown-unknown
                -nostdlib
                -O2
                -Wl,--no-entry
                -Wl,--export=module_step
                -Wl,--export=module_init
                -Wl,--allow-undefined
                -o "${MODULE_WASM}"
                "${CMAKE_CURRENT_SOURCE_DIR}/module/${MODULE_NAME}.c"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/module/${MODULE_NAME}.c"
        COMMENT "[demo:${DEMO_NAME}] C → WASM"
    )

    add_custom_command(
        OUTPUT  "${MODULE_AOT}"
        COMMAND "${WAMRC}"
                --target=x86_64
                --target-abi=gnu
                -o "${MODULE_AOT}"
                "${MODULE_WASM}"
        DEPENDS "${MODULE_WASM}"
        COMMENT "[demo:${DEMO_NAME}] WASM → AOT"
    )

    add_custom_target(${DEMO_NAME}_aot ALL DEPENDS "${MODULE_AOT}")
else()
    message(WARNING
        "[demo:${DEMO_NAME}] wasi-clang or wamrc missing — skipping WASM/AOT.\n"
        "  Build wamrc: cd ${FUSE_DIR}/wasm-micro-runtime/wamr-compiler\n"
        "               ./build_llvm.sh --arch X86\n"
        "               mkdir build && cd build && cmake .. && make -j\$(nproc)"
    )
endif()

# ---------------------------------------------------------------------------
# Host executable
# ---------------------------------------------------------------------------
add_executable(${DEMO_NAME}_host host/main.c)
target_link_libraries(${DEMO_NAME}_host PRIVATE fuse)
# _GNU_SOURCE enables POSIX + GNU extensions (clock_gettime, sigaction, usleep)
# on Linux.  Demo hosts are Linux-specific; this is intentional.
target_compile_definitions(${DEMO_NAME}_host PRIVATE _GNU_SOURCE)
target_compile_options(${DEMO_NAME}_host PRIVATE
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O2>
)

if(WASI_CLANG AND WAMRC)
    add_dependencies(${DEMO_NAME}_host ${DEMO_NAME}_aot)
endif()
