#!/usr/bin/env python3
"""
gen_app_config.py — Generate FUSE application configuration artifacts.

Reads an app_config.json file and produces:
  1. fuse_app_config.h      — C header with all application and per-module constants
  2. fuse_hal_flags.cmake   — CMake set() variables for enabled HAL groups
  3. <module>_policy.bin    — 24-byte fuse_policy_t binary per module (little-endian)

Usage:
    python3 gen_app_config.py --input app_config.json --output-dir <dir>
"""

import argparse
import json
import re
import struct
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Mappings
# ---------------------------------------------------------------------------

HAL_GROUP_TO_FLAG = {
    "temp_sensor": "FUSE_HAL_ENABLE_TEMP_SENSOR",
    "timer":       "FUSE_HAL_ENABLE_TIMER",
    "camera":      "FUSE_HAL_ENABLE_CAMERA",
}

# Capability string → bitmask bit value
CAP_BITS = {
    "TEMP_SENSOR": 0x01,
    "TIMER":       0x02,
    "CAMERA":      0x04,
    "LOG":         0x08,
}

# Capability → required hal_group (LOG has no hal_group requirement)
CAP_TO_HAL_GROUP = {
    "TEMP_SENSOR": "temp_sensor",
    "TIMER":       "timer",
    "CAMERA":      "camera",
}

FUSE_MAX_MODULES = 8

# ---------------------------------------------------------------------------
# Validation helpers
# ---------------------------------------------------------------------------

def validate(config: dict, input_path: str) -> None:
    """Validate the app_config structure; raises SystemExit on error."""

    def err(msg: str) -> None:
        print(f"ERROR [{input_path}]: {msg}", file=sys.stderr)
        sys.exit(1)

    if "application" not in config:
        err("missing top-level 'application' key")

    app = config["application"]
    for required in ("name", "max_modules", "wamr_pool_bytes", "log_pool_bytes", "hal_groups"):
        if required not in app:
            err(f"application.{required} is required")

    # hal_groups must only contain valid hardware group names (not 'log')
    hal_groups = app["hal_groups"]
    if not isinstance(hal_groups, list):
        err("application.hal_groups must be a list")
    for g in hal_groups:
        if g == "log":
            err("'log' must not appear in hal_groups — it is always registered")
        if g not in HAL_GROUP_TO_FLAG:
            err(f"unknown hal_group '{g}'; valid values: {list(HAL_GROUP_TO_FLAG)}")

    # max_modules
    max_modules = app["max_modules"]
    if not isinstance(max_modules, int) or max_modules < 1 or max_modules > FUSE_MAX_MODULES:
        err(f"application.max_modules must be 1..{FUSE_MAX_MODULES}, got {max_modules}")

    # modules list — optional; omit for pure dynamic deployments
    if "modules" not in config:
        return  # platform-only config is valid; no module entries to validate

    modules = config["modules"]
    if not isinstance(modules, list) or len(modules) == 0:
        err("modules must be a non-empty list")
    if len(modules) > max_modules:
        err(f"{len(modules)} modules defined but max_modules={max_modules}")

    hal_group_set = set(hal_groups)
    for mod in modules:
        for required in ("name", "binary", "policy"):
            if required not in mod:
                err(f"module missing required field '{required}'")

        name = mod["name"]
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            err(f"module name '{name}' is not a valid C identifier")

        policy = mod["policy"]
        caps = policy.get("capabilities", [])
        if not isinstance(caps, list):
            err(f"module '{name}': policy.capabilities must be a list of strings")

        for cap in caps:
            if cap not in CAP_BITS:
                err(f"module '{name}': unknown capability '{cap}'; "
                    f"valid: {list(CAP_BITS)}")
            # LOG is always valid; other caps require the hal_group to be present
            if cap in CAP_TO_HAL_GROUP:
                required_group = CAP_TO_HAL_GROUP[cap]
                if required_group not in hal_group_set:
                    err(f"module '{name}' requests capability '{cap}' "
                        f"but hal_group '{required_group}' is not in "
                        f"application.hal_groups")

        for field in ("memory_pages_max", "stack_size", "heap_size",
                      "cpu_quota_us", "step_interval_us"):
            if field not in policy:
                err(f"module '{name}': policy.{field} is required")
            if not isinstance(policy[field], int) or policy[field] < 0:
                err(f"module '{name}': policy.{field} must be a non-negative integer")


# ---------------------------------------------------------------------------
# Code generation helpers
# ---------------------------------------------------------------------------

def caps_to_bitmask(caps: list) -> int:
    result = 0
    for cap in caps:
        result |= CAP_BITS[cap]
    return result


def module_macro_prefix(name: str) -> str:
    """Convert module name to uppercase C macro prefix."""
    return "FUSE_POLICY_" + re.sub(r"[^A-Za-z0-9]", "_", name).upper() + "_"


def generate_header(config: dict, input_path: str) -> str:
    app = config["application"]
    modules = config.get("modules", [])
    hal_groups = app["hal_groups"]

    lines = [
        "/* Auto-generated from app_config.json by tools/gen_app_config.py — DO NOT EDIT */",
        f"/* Source: {input_path} */",
        "#pragma once",
        "",
        "/* Application-level constants */",
        f'#define FUSE_APP_NAME            "{app["name"]}"',
        f'#define FUSE_APP_MAX_MODULES      {app["max_modules"]}U',
        f'#define FUSE_APP_WAMR_POOL_BYTES  {app["wamr_pool_bytes"]}U',
        f'#define FUSE_APP_LOG_POOL_BYTES   {app["log_pool_bytes"]}U',
        "",
        "/* HAL groups enabled on this platform */",
    ]

    for group in hal_groups:
        flag = HAL_GROUP_TO_FLAG[group]
        lines.append(f"#define {flag}  1")

    for mod in modules:
        name = mod["name"]
        policy = mod["policy"]
        caps = policy["capabilities"]
        bitmask = caps_to_bitmask(caps)
        cap_comment = " | ".join(caps) + f" = 0x{bitmask:02X}"
        prefix = module_macro_prefix(name)

        lines += [
            "",
            f"/* ---- Module: {name} {'-' * max(0, 60 - len(name))} */",
            f"/* policy capabilities bitmask: {cap_comment} */",
            f"#define {prefix}CAPABILITIES      {bitmask}U",
            f"#define {prefix}MEMORY_PAGES_MAX  {policy['memory_pages_max']}U",
            f"#define {prefix}STACK_SIZE        {policy['stack_size']}U",
            f"#define {prefix}HEAP_SIZE         {policy['heap_size']}U",
            f"#define {prefix}CPU_QUOTA_US      {policy['cpu_quota_us']}U",
            f"#define {prefix}STEP_INTERVAL_US  {policy['step_interval_us']}U",
            f'#define {prefix}BINARY            "{mod["binary"]}"',
        ]

    return "\n".join(lines) + "\n"


def generate_cmake_flags(config: dict) -> str:
    hal_groups = config["application"]["hal_groups"]
    lines = [
        "# Auto-generated from app_config.json by tools/gen_app_config.py — DO NOT EDIT",
    ]
    for group in hal_groups:
        flag = HAL_GROUP_TO_FLAG[group]
        lines.append(f"set({flag}  1)")
    return "\n".join(lines) + "\n"


def generate_policy_bin(policy: dict) -> bytes:
    """Pack fuse_policy_t as 6 × little-endian uint32_t (24 bytes)."""
    caps = policy["capabilities"]
    bitmask = caps_to_bitmask(caps)
    return struct.pack(
        "<6I",
        bitmask,
        policy["memory_pages_max"],
        policy["stack_size"],
        policy["heap_size"],
        policy["cpu_quota_us"],
        policy["step_interval_us"],
    )


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate FUSE application configuration artifacts from app_config.json."
    )
    parser.add_argument("--input",      required=True, help="Path to app_config.json")
    parser.add_argument("--output-dir", required=True, help="Directory for generated files")
    args = parser.parse_args()

    input_path = Path(args.input).resolve()
    output_dir = Path(args.output_dir)

    if not input_path.is_file():
        print(f"ERROR: input file not found: {input_path}", file=sys.stderr)
        sys.exit(1)

    try:
        with open(input_path) as f:
            config = json.load(f)
    except json.JSONDecodeError as exc:
        print(f"ERROR: failed to parse JSON: {exc}", file=sys.stderr)
        sys.exit(1)

    validate(config, str(input_path))

    output_dir.mkdir(parents=True, exist_ok=True)

    # 1. fuse_app_config.h
    header_path = output_dir / "fuse_app_config.h"
    header_path.write_text(generate_header(config, str(input_path)))
    print(f"Generated: {header_path}")

    # 2. fuse_hal_flags.cmake
    cmake_path = output_dir / "fuse_hal_flags.cmake"
    cmake_path.write_text(generate_cmake_flags(config))
    print(f"Generated: {cmake_path}")

    # 3. Per-module policy binaries (only when modules section is present)
    modules = config.get("modules", [])
    for mod in modules:
        bin_name = f"{mod['name']}_policy.bin"
        bin_path = output_dir / bin_name
        bin_path.write_bytes(generate_policy_bin(mod["policy"]))
        print(f"Generated: {bin_path}")

    print(f"\nSuccess: {2 + len(modules)} files written to {output_dir}")


if __name__ == "__main__":
    main()
