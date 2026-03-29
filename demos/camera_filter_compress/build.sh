#!/usr/bin/env bash
# =============================================================================
# build.sh — Build the camera_filter_compress demo as a standalone project.
#
# Usage:
#   ./build.sh [options]
#
# Options:
#   --clean              Remove the build directory before configuring
#   --debug              Build with Debug configuration (default: Release)
#   --fuse-dir <path>    Override path to the fuse repository root
#                        (default: two levels up from this script)
#   --build-dir <path>   Override build directory (default: ./build)
#
# Examples:
#   ./build.sh
#   ./build.sh --clean --debug
#   ./build.sh --fuse-dir /opt/fuse
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FUSE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BUILD_TYPE="Release"
CLEAN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)
            CLEAN=1
            ;;
        --debug)
            BUILD_TYPE="Debug"
            ;;
        --fuse-dir)
            FUSE_DIR="$(cd "$2" && pwd)"
            shift
            ;;
        --build-dir)
            BUILD_DIR="$2"
            shift
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 [--clean] [--debug] [--fuse-dir <path>] [--build-dir <path>]" >&2
            exit 1
            ;;
    esac
    shift
done

if [[ "${CLEAN}" -eq 1 && -d "${BUILD_DIR}" ]]; then
    echo "[build.sh] Removing ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
fi

echo "[build.sh] fuse root  : ${FUSE_DIR}"
echo "[build.sh] build dir  : ${BUILD_DIR}"
echo "[build.sh] build type : ${BUILD_TYPE}"
echo ""

cmake \
    -S "${SCRIPT_DIR}" \
    -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DFUSE_DIR="${FUSE_DIR}"

cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}"

echo ""
echo "[build.sh] Build complete."
echo ""
echo "Run the demo:"
echo "  ${BUILD_DIR}/camera_filter_compress_host \\"
echo "    ${BUILD_DIR}/out/frame_filter.aot \\"
echo "    ${BUILD_DIR}/out/frame_compress.aot"
