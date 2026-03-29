# =============================================================================
# platform/linux/CMakeLists.txt — Linux POSIX platform implementation
#
# Builds fuse_platform_linux (aliased as fuse_platform) from the canonical
# Linux SIGALRM-based quota implementation.  Consumers link against
# fuse_platform and receive platform.h via the PUBLIC include directory.
# =============================================================================

add_library(fuse_platform_linux STATIC platform.c)

target_include_directories(fuse_platform_linux
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..   # platform/platform.h
)

target_compile_definitions(fuse_platform_linux PRIVATE _GNU_SOURCE)

# Link PUBLIC so that consumers of fuse_platform_linux also get fuse's
# transitive include directories (fuse.h, fuse_types.h, fuse_app_config.h).
target_link_libraries(fuse_platform_linux PUBLIC fuse)

# Note: the 'fuse_platform' alias is created by the root CMakeLists.txt after
# all platform subdirectories are added, so it always resolves to the correct
# implementation for the selected FUSE_PLATFORM.
