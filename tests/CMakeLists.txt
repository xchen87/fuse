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
    mod_temp_no_cap
    mod_timer
    mod_timer_no_cap
    mod_camera
    mod_camera_oob
    mod_camera_no_cap
    mod_camera_zero_len
    mod_log
    mod_log_long
    mod_log_oob
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
# Explicitly enable all HAL groups for tests — tests exercise all capabilities
# regardless of any application-level FUSE_APP_CONFIG setting.
target_compile_definitions(fuse_test PRIVATE
    FUSE_AOT_DIR="${FUSE_AOT_DIR}"
    FUSE_HAL_ENABLE_TEMP_SENSOR=1
    FUSE_HAL_ENABLE_TIMER=1
    FUSE_HAL_ENABLE_CAMERA=1
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

# ---------------------------------------------------------------------------
# Platform integration tests
# ---------------------------------------------------------------------------

# Linux platform tests — always built when fuse is the top-level project on Linux.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(fuse_platform_test
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/test_platform_linux.cpp
    )
    # Link fuse_platform_linux directly — not the alias — so this test always
    # uses the Linux POSIX implementation regardless of FUSE_PLATFORM.
    target_link_libraries(fuse_platform_test PRIVATE
        GTest::gtest_main
        fuse_platform_linux
    )
    target_include_directories(fuse_platform_test PRIVATE
        ${CMAKE_SOURCE_DIR}          # so "platform/platform.h" resolves
        ${CMAKE_SOURCE_DIR}/include  # fuse.h
    )
    gtest_discover_tests(fuse_platform_test
        PROPERTIES LABELS "platform"
    )
endif()

# FreeRTOS platform tests — built when FUSE_PLATFORM=freertos.
if(FUSE_PLATFORM STREQUAL "freertos")
    add_executable(fuse_platform_freertos_test
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/freertos/test_platform_freertos.cpp
    )
    # Link fuse_platform_freertos directly — not the alias.
    target_link_libraries(fuse_platform_freertos_test PRIVATE
        GTest::gtest
        GTest::gmock
        fuse_platform_freertos
    )
    target_include_directories(fuse_platform_freertos_test PRIVATE
        ${CMAKE_SOURCE_DIR}          # "platform/platform.h"
        ${CMAKE_SOURCE_DIR}/include  # fuse.h
    )
    # Custom main in test file — do not use gtest_main.
    add_test(NAME PlatformFreeRTOS COMMAND fuse_platform_freertos_test)
    set_tests_properties(PlatformFreeRTOS PROPERTIES LABELS "platform")
endif()
