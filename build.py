#!/usr/bin/env python3

import argparse
import subprocess
import shutil
import os
import sys

def run_cmd(cmd, cwd=None):
    print(f"[CMD] {' '.join(cmd)}")
    try:
        subprocess.check_call(cmd, cwd=cwd)
    except subprocess.CalledProcessError as e:
        print(f"Error: command failed with exit code {e.returncode}")
        sys.exit(e.returncode)

def parse_args():
    parser = argparse.ArgumentParser(description="CMake build helper")

    parser.add_argument(
            "-c",
            "--clean",
            action="store_true",
            help="Clean build directory before building",
            )

    parser.add_argument(
            "-v",
            "--verbose",
            action="store_true",
            help="Enable verbose build (CMAKE_VERBOSE_MAKEFILE)",
            )

    parser.add_argument(
            "-b",
            "--build_config",
            choices=["Release", "Debug"],
            default="Release",
            help="Build configuration",
            )

    parser.add_argument(
            "-o",
            "--output_dir",
            default="build",
            help="Build output directory (default: build)",
            )

    parser.add_argument(
            "-p",
            "--platform",
            choices=["linux", "freertos"],
            default="linux",
            help="Target platform (default: linux)",
            )

    args = parser.parse_args()
    return args

def main():
    args = parse_args()

    build_dir = os.path.abspath(args.output_dir)

    # Clean if requested
    if args.clean and os.path.exists(build_dir):
        print(f"[INFO] Cleaning build directory: {build_dir}")
        shutil.rmtree(build_dir)

    # Ensure build directory exists
    os.makedirs(build_dir, exist_ok=True)

    # Configure step (cmake)
    cmake_cmd = [
            "cmake",
            "-S", ".",
            "-B", build_dir,
            f"-DCMAKE_BUILD_TYPE={args.build_config}",
            f"-DFUSE_PLATFORM={args.platform}",
    ]

    if args.verbose:
        cmake_cmd.append("-DCMAKE_VERBOSE_MAKEFILE=ON")

    run_cmd(cmake_cmd)

    # Build step
    build_cmd = [
            "cmake",
            "--build",
            build_dir,
            "--config",
            args.build_config,
            ]

    run_cmd(build_cmd)
    print("[INFO] Build completed successfully!")

if __name__ == "__main__":
    main()
