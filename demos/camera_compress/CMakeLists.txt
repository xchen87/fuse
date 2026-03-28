# =============================================================================
# demos/camera_compress — Camera frame RLE compression demo
#
# Compiles a freestanding WASM module (camera_compress.c) to AOT, converts
# the policy JSON to a binary, and builds the host executable.
#
# Added as a subdirectory by the root CMakeLists.txt so it reuses the parent
# project's 'fuse' CMake target directly.
#
# Toolchain requirements (same as tests/):
#   wasi-clang : /opt/wasi-sdk/bin/clang
#   wamrc      : wasm-micro-runtime/wamr-compiler/build/wamrc (built separately)
#   python3    : for policy JSON → binary conversion
# =============================================================================

message(STATUS "[demo:camera_compress] Configuring...")

# ---------------------------------------------------------------------------
# Locate toolchain
# ---------------------------------------------------------------------------
find_program(WASI_CLANG clang HINTS /opt/wasi-sdk/bin NO_DEFAULT_PATH)
find_program(WAMRC wamrc HINTS
    ${CMAKE_SOURCE_DIR}/wasm-micro-runtime/wamr-compiler/build
    /opt/wamrc
    /usr/local/bin
)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

if(WASI_CLANG)
    message(STATUS "[demo:camera_compress] wasi-clang : ${WASI_CLANG}")
else()
    message(WARNING "[demo:camera_compress] wasi-clang not found — WASM module will NOT be compiled")
endif()

if(WAMRC)
    message(STATUS "[demo:camera_compress] wamrc      : ${WAMRC}")
else()
    message(WARNING "[demo:camera_compress] wamrc not found — AOT binary will NOT be generated")
endif()

# ---------------------------------------------------------------------------
# Output directory for generated binaries
# ---------------------------------------------------------------------------
set(DEMO_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/out")
file(MAKE_DIRECTORY "${DEMO_OUT_DIR}")

# ---------------------------------------------------------------------------
# Policy JSON → binary  (always built — only needs python3)
# ---------------------------------------------------------------------------
set(POLICY_BIN "${DEMO_OUT_DIR}/camera_compress_policy.bin")

add_custom_command(
    OUTPUT  "${POLICY_BIN}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/policy_to_bin.py"
            --input  "${CMAKE_CURRENT_SOURCE_DIR}/policy/camera_compress_policy.json"
            --output "${POLICY_BIN}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/policy/camera_compress_policy.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/policy_to_bin.py"
    COMMENT "[demo:camera_compress] policy JSON → binary"
)
add_custom_target(camera_compress_policy ALL DEPENDS "${POLICY_BIN}")

# ---------------------------------------------------------------------------
# WASM module: C → .wasm → .aot
# ---------------------------------------------------------------------------
if(WASI_CLANG AND WAMRC)
    set(MODULE_WASM "${DEMO_OUT_DIR}/camera_compress.wasm")
    set(MODULE_AOT  "${DEMO_OUT_DIR}/camera_compress.aot")

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
                "${CMAKE_CURRENT_SOURCE_DIR}/module/camera_compress.c"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/module/camera_compress.c"
        COMMENT "[demo:camera_compress] C → WASM"
    )

    add_custom_command(
        OUTPUT  "${MODULE_AOT}"
        COMMAND "${WAMRC}"
                --target=x86_64
                --target-abi=gnu
                -o "${MODULE_AOT}"
                "${MODULE_WASM}"
        DEPENDS "${MODULE_WASM}"
        COMMENT "[demo:camera_compress] WASM → AOT"
    )

    add_custom_target(camera_compress_aot ALL DEPENDS "${MODULE_AOT}")
    message(STATUS "[demo:camera_compress] WASM toolchain found — module will be compiled to AOT")
else()
    message(WARNING
        "[demo:camera_compress] wasi-clang or wamrc missing — skipping WASM/AOT compilation.\n"
        "  To build wamrc:\n"
        "    cd wasm-micro-runtime/wamr-compiler && ./build_llvm.sh --arch X86\n"
        "    mkdir build && cd build && cmake .. && make -j\$(nproc)\n"
    )
endif()

# ---------------------------------------------------------------------------
# Host executable
# ---------------------------------------------------------------------------
add_executable(camera_compress_host host/main.c)

target_include_directories(camera_compress_host PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

target_link_libraries(camera_compress_host PRIVATE fuse)

target_compile_options(camera_compress_host PRIVATE
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O2>
)

# Host depends on policy binary (always) and AOT (when toolchain present)
add_dependencies(camera_compress_host camera_compress_policy)
if(WASI_CLANG AND WAMRC)
    add_dependencies(camera_compress_host camera_compress_aot)
endif()

# Print paths so the user knows how to run the demo
if(WASI_CLANG AND WAMRC)
    message(STATUS
        "[demo:camera_compress] Run after build:\n"
        "  ./build/demos/camera_compress/camera_compress_host \\\n"
        "    ./build/demos/camera_compress/out/camera_compress.aot \\\n"
        "    ./build/demos/camera_compress/out/camera_compress_policy.bin"
    )
endif()
