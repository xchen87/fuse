# =============================================================================
# platform/freertos/CMakeLists.txt — FreeRTOS POSIX simulator platform
#
# Fetches the FreeRTOS kernel and builds it with the POSIX port so that the
# FreeRTOS API (tasks, timers, etc.) is available on x86 Linux for CI testing.
# Exposes fuse_platform_freertos (aliased as fuse_platform) to consumers.
# =============================================================================

cmake_minimum_required(VERSION 3.14)

include(FetchContent)
FetchContent_Declare(
    freertos_kernel
    GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG        V11.1.0
    GIT_SHALLOW    TRUE
)

# FreeRTOS kernel V11+ requires a 'freertos_config' INTERFACE target that
# points to the directory containing FreeRTOSConfig.h before MakeAvailable.
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}   # FreeRTOSConfig.h lives here
)
target_compile_definitions(freertos_config INTERFACE projCOVERAGE_TEST=0)

FetchContent_MakeAvailable(freertos_kernel)

set(FREERTOS_POSIX_PORT_DIR
    "${freertos_kernel_SOURCE_DIR}/portable/ThirdParty/GCC/Posix")

add_library(fuse_platform_freertos STATIC
    platform.c
    ${freertos_kernel_SOURCE_DIR}/tasks.c
    ${freertos_kernel_SOURCE_DIR}/queue.c
    ${freertos_kernel_SOURCE_DIR}/list.c
    ${freertos_kernel_SOURCE_DIR}/timers.c
    ${freertos_kernel_SOURCE_DIR}/portable/MemMang/heap_4.c
    ${FREERTOS_POSIX_PORT_DIR}/port.c
    ${FREERTOS_POSIX_PORT_DIR}/utils/wait_for_event.c
)

target_include_directories(fuse_platform_freertos
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}          # FreeRTOSConfig.h
        ${CMAKE_CURRENT_SOURCE_DIR}/..       # platform/platform.h
        ${freertos_kernel_SOURCE_DIR}/include
        ${FREERTOS_POSIX_PORT_DIR}
)

target_compile_definitions(fuse_platform_freertos PRIVATE _GNU_SOURCE)

# Link PUBLIC so consumers get fuse's transitive headers (fuse.h, fuse_types.h).
# pthread is required by the FreeRTOS POSIX port.
target_link_libraries(fuse_platform_freertos PUBLIC fuse pthread)

# Note: the 'fuse_platform' alias is created by the root CMakeLists.txt after
# all platform subdirectories are added, so it always resolves to the correct
# implementation for the selected FUSE_PLATFORM.
