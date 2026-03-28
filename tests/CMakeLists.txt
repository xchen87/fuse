# =============================================================================
# tests/CMakeLists.txt — FUSE test suite build configuration
#
# Toolchain requirements:
#   wat2wasm  : /opt/wabt/bin/wat2wasm         (ships with wabt in docker image)
#   wasi-clang: /opt/wasi-sdk/bin/clang        (ships with wasi-sdk in docker image)
#   wamrc     : found via find_program(WAMRC …) — must be built separately
#               from wasm-micro-runtime/wamr-compiler/ before running tests
#               that exercise real module execution.
#
# If wamrc is not found, the .aot generation steps are skipped and every test
# that calls LoadAotOrSkip() will emit GTEST_SKIP() at runtime.
# =============================================================================

cmake_minimum_required(VERSION 3.14)

# ---------------------------------------------------------------------------
# Locate toolchain binaries
# ---------------------------------------------------------------------------
find_program(WAT2WASM  wat2wasm  HINTS /opt/wabt/bin   NO_DEFAULT_PATH)
find_program(WASI_CLANG clang    HINTS /opt/wasi-sdk/bin NO_DEFAULT_PATH)
find_program(WAMRC     wamrc     HINTS
    ${CMAKE_SOURCE_DIR}/wasm-micro-runtime/wamr-compiler/build
    /opt/wamrc
    /usr/local/bin
)

if(WAT2WASM)
    message(STATUS "[FUSE tests] wat2wasm  : ${WAT2WASM}")
else()
    message(WARNING "[FUSE tests] wat2wasm not found — WAT modules cannot be compiled")
endif()

if(WASI_CLANG)
    message(STATUS "[FUSE tests] wasi-clang: ${WASI_CLANG}")
else()
    message(WARNING "[FUSE tests] wasi-sdk clang not found")
endif()

if(WAMRC)
    message(STATUS "[FUSE tests] wamrc     : ${WAMRC}")
else()
    message(WARNING "[FUSE tests] wamrc not found — AOT binaries will NOT be generated.\n"
                    "             Module-execution tests will be skipped at runtime.\n"
                    "             To build wamrc:\n"
                    "               cd wasm-micro-runtime/wamr-compiler\n"
                    "               ./build_llvm.sh --arch X86\n"
                    "               mkdir build && cd build\n"
                    "               cmake .. && make -j$(nproc)\n")
endif()

# ---------------------------------------------------------------------------
# AOT binary output directory (inside the build tree)
# ---------------------------------------------------------------------------
set(FUSE_AOT_DIR "${CMAKE_CURRENT_BINARY_DIR}/aot")
file(MAKE_DIRECTORY "${FUSE_AOT_DIR}")

# ---------------------------------------------------------------------------
# Helper macro: compile one .wat → .wasm → .aot
#
# Usage:
#   add_wat_aot_target(TARGET_NAME  wat_src_path  aot_output_path)
#
# Creates a custom target TARGET_NAME that produces aot_output_path.
# Depends on WAT2WASM and WAMRC being available; skipped gracefully otherwise.
# ---------------------------------------------------------------------------
macro(add_wat_aot_target target_name wat_src aot_out)
    if(WAT2WASM AND WAMRC)
        get_filename_component(_base "${aot_out}" NAME_WE)
        set(_wasm_out "${FUSE_AOT_DIR}/${_base}.wasm")

        add_custom_command(
            OUTPUT  "${_wasm_out}"
            COMMAND "${WAT2WASM}" "${wat_src}" -o "${_wasm_out}"
            DEPENDS "${wat_src}"
            COMMENT "[FUSE tests] wat2wasm  ${wat_src} → ${_wasm_out}"
        )

        add_custom_command(
            OUTPUT  "${aot_out}"
            COMMAND "${WAMRC}"
                    --target=x86_64
                    --target-abi=gnu
                    -o "${aot_out}"
                    "${_wasm_out}"
            DEPENDS "${_wasm_out}"
            COMMENT "[FUSE tests] wamrc     ${_wasm_out} → ${aot_out}"
        )

        add_custom_target("${target_name}" ALL DEPENDS "${aot_out}")
    endif()
endmacro()

# ---------------------------------------------------------------------------
# List of WAT module sources → AOT binaries
# ---------------------------------------------------------------------------
set(MODULES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/modules")

set(WAT_MODULES
    mod_step_only
    mod_with_init_deinit
    mod_no_step
    mod_temp
    mod_timer
    mod_camera
    mod_camera_oob
    mod_log
    mod_log_long
)

foreach(_mod ${WAT_MODULES})
    add_wat_aot_target(
        "aot_${_mod}"
        "${MODULES_DIR}/${_mod}.wat"
        "${FUSE_AOT_DIR}/${_mod}.aot"
    )
endforeach()

# ---------------------------------------------------------------------------
# Test executable
# ---------------------------------------------------------------------------
file(GLOB TEST_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(fuse_test ${TEST_SRCS})

target_link_libraries(fuse_test PRIVATE
    GTest::gtest_main
    GTest::gmock
    fuse
)

target_include_directories(fuse_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/core"
    "${CMAKE_SOURCE_DIR}/wasm-micro-runtime/core/iwasm/include"
)

# Tell test helper header where .aot files live at runtime.
target_compile_definitions(fuse_test PRIVATE
    FUSE_AOT_DIR="${FUSE_AOT_DIR}"
)

# If AOT binaries were requested, make the test target depend on them.
if(WAT2WASM AND WAMRC)
    foreach(_mod ${WAT_MODULES})
        add_dependencies(fuse_test "aot_${_mod}")
    endforeach()
endif()

# ---------------------------------------------------------------------------
# CTest integration
# ---------------------------------------------------------------------------
include(GoogleTest)
gtest_discover_tests(fuse_test)
