cmake_minimum_required (VERSION 3.14)

project(fuse C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build Type" FORCE)
endif()
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")

set(WAMR_BUILD_PLATFORM "linux")
set(WAMR_BUILD_TARGET "X86_64")
set(WAMR_BUILD_INTERP 0)
set(WAMR_BUILD_FAST_INTERP 0)
set(WAMR_BUILD_AOT 1)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_SIMD 0)
set(WAMR_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/wasm-micro-runtime)

include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
add_library(vmlib ${WAMR_RUNTIME_LIB_SOURCE})

# ---------------------------------------------------------------------------
# GoogleTest and unit tests — only when fuse is the top-level project.
# When a demo adds fuse via add_subdirectory(), this block is skipped so
# GoogleTest is not fetched and tests are not added to the demo's build.
# ---------------------------------------------------------------------------
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
    )
    FetchContent_MakeAvailable(googletest)
    enable_testing()
endif()

file(GLOB SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/core/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/temp/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/timer/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/camera/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/log/*.c"
)
add_library(fuse SHARED ${SRCS})
target_include_directories(fuse
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core
            ${WAMR_ROOT_DIR}/core/iwasm/include
)
target_link_libraries(fuse PUBLIC vmlib -lm)
target_compile_options(fuse PRIVATE
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3>
)

# ---------------------------------------------------------------------------
# Application-level HAL group configuration
#
# Set -DFUSE_APP_CONFIG=<path/to/app_config.json> at cmake configure time to
# compile FUSE with only the hardware groups declared in that application's
# configuration.  If unset, all groups are enabled (suitable for test builds).
#
# The FUSE_HAL_ENABLE_* flags are propagated PUBLIC so that consuming targets
# (demo host apps, test executables) see the same struct layout for fuse_hal_t.
# ---------------------------------------------------------------------------
set(FUSE_APP_CONFIG "" CACHE PATH
    "Path to an application app_config.json; if empty, all HAL groups are enabled.")

find_package(Python3 COMPONENTS Interpreter REQUIRED)

if(FUSE_APP_CONFIG)
    message(STATUS "[fuse] App config: ${FUSE_APP_CONFIG}")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_app_config.py"
                --input  "${FUSE_APP_CONFIG}"
                --output-dir "${CMAKE_BINARY_DIR}/generated"
        RESULT_VARIABLE _gen_result
        OUTPUT_VARIABLE _gen_output
        ERROR_VARIABLE  _gen_error
    )
    if(NOT _gen_result EQUAL 0)
        message(FATAL_ERROR "[fuse] gen_app_config.py failed:\n${_gen_error}")
    endif()
    include("${CMAKE_BINARY_DIR}/generated/fuse_hal_flags.cmake")
    target_include_directories(fuse PUBLIC "${CMAKE_BINARY_DIR}/generated")
else()
    message(STATUS "[fuse] No FUSE_APP_CONFIG — enabling all HAL groups")
    set(FUSE_HAL_ENABLE_TEMP_SENSOR 1)
    set(FUSE_HAL_ENABLE_TIMER       1)
    set(FUSE_HAL_ENABLE_CAMERA      1)
endif()

foreach(_grp TEMP_SENSOR TIMER CAMERA)
    if(FUSE_HAL_ENABLE_${_grp})
        target_compile_definitions(fuse PUBLIC "FUSE_HAL_ENABLE_${_grp}=1")
        message(STATUS "[fuse] HAL group enabled: ${_grp}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Tests — only when fuse is the top-level project (demos build separately)
# ---------------------------------------------------------------------------
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    add_subdirectory(tests)
endif()
