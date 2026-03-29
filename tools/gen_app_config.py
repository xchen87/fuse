#!/usr/bin/env python3
"""
gen_app_config.py — Generate FUSE application configuration artifacts.

Reads an app_config.json file and produces:
  1. fuse_app_config.h      — C header with all application and per-module constants
  2. fuse_hal_flags.cmake   — CMake set() variables for enabled HAL groups
  3. <module>_policy.bin    — 32-byte fuse_policy_t binary per module (little-endian)

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
    "EVENT_POST":  0x10,
}

# Capability → required hal_group (LOG and EVENT_POST have no hal_group requirement)
CAP_TO_HAL_GROUP = {
    "TEMP_SENSOR": "temp_sensor",
    "TIMER":       "timer",
    "CAMERA":      "camera",
}

# Activation mode string → bit value
ACTIVATION_BITS = {
    "INTERVAL": 0x01,
    "EVENT":    0x02,
    "MANUAL":   0x04,
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

    # application.events — optional dict of event name → bit index (0-31)
    events = app.get("events", {})
    if not isinstance(events, dict):
        err("application.events must be a dict of name -> bit_index (0-31)")
    seen_event_bits = set()
    for ev_name, ev_bit in events.items():
        if not isinstance(ev_bit, int) or ev_bit < 0 or ev_bit > 31:
            err(f"application.events['{ev_name}']: bit index must be 0-31, got {ev_bit}")
        if ev_bit in seen_event_bits:
            err(f"application.events['{ev_name}']: bit index {ev_bit} is already used by another event")
        seen_event_bits.add(ev_bit)

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
            # LOG and EVENT_POST are always valid; other caps require the hal_group to be present
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

        # activation — optional list of strings; default ["INTERVAL"]
        activation_list = policy.get("activation", ["INTERVAL"])
        if not isinstance(activation_list, list):
            err(f"module '{name}': policy.activation must be a list of strings")
        for act in activation_list:
            if act not in ACTIVATION_BITS:
                err(f"module '{name}': unknown activation mode '{act}'; "
                    f"valid: {list(ACTIVATION_BITS)}")

        # event_subscribe — optional list of event names or integers; default []
        event_sub = policy.get("event_subscribe", [])
        if not isinstance(event_sub, list):
            err(f"module '{name}': policy.event_subscribe must be a list")
        for ev in event_sub:
            if isinstance(ev, int):
                if ev < 0 or ev > 31:
                    err(f"module '{name}': event_subscribe integer {ev} must be 0-31")
            elif isinstance(ev, str):
                if ev not in events:
                    err(f"module '{name}': event_subscribe references unknown event '{ev}'; "
                        f"defined events: {list(events)}")
            else:
                err(f"module '{name}': event_subscribe entries must be strings or integers")

        # Warn if EVENT activation is set but event_subscribe is empty
        if "EVENT" in activation_list and len(event_sub) == 0:
            print(f"WARNING [{input_path}]: module '{name}' has FUSE_ACTIVATION_EVENT "
                  f"but no event_subscribe entries — event-triggered steps will never fire",
                  file=sys.stderr)


# ---------------------------------------------------------------------------
# Code generation helpers
# ---------------------------------------------------------------------------

def caps_to_bitmask(caps: list) -> int:
    result = 0
    for cap in caps:
        result |= CAP_BITS[cap]
    return result


def activation_to_bitmask(activation_list: list) -> int:
    result = 0
    for act in activation_list:
        result |= ACTIVATION_BITS[act]
    return result


def event_subscribe_to_bitmask(event_sub: list, events_map: dict) -> int:
    """Convert list of event names/integers to a bitmask."""
    result = 0
    for ev in event_sub:
        if isinstance(ev, int):
            result |= (1 << ev)
        else:
            # ev is a string name; look up bit index from events_map
            result |= (1 << events_map[ev])
    return result


def module_macro_prefix(name: str) -> str:
    """Convert module name to uppercase C macro prefix."""
    return "FUSE_POLICY_" + re.sub(r"[^A-Za-z0-9]", "_", name).upper() + "_"


def generate_header(config: dict, input_path: str) -> str:
    app = config["application"]
    modules = config.get("modules", [])
    hal_groups = app["hal_groups"]
    events = app.get("events", {})

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

    if events:
        lines += [
            "",
            "/* ---- Application-defined events ---- */",
        ]
        for ev_name, ev_bit in sorted(events.items(), key=lambda x: x[1]):
            macro_name = "FUSE_EVENT_" + re.sub(r"[^A-Za-z0-9]", "_", ev_name).upper()
            lines.append(f"#define {macro_name}  {ev_bit}u")

    for mod in modules:
        name = mod["name"]
        policy = mod["policy"]
        caps = policy["capabilities"]
        bitmask = caps_to_bitmask(caps)
        cap_comment = " | ".join(caps) + f" = 0x{bitmask:02X}"
        prefix = module_macro_prefix(name)

        activation_list = policy.get("activation", ["INTERVAL"])
        activation_mask = activation_to_bitmask(activation_list)
        act_comment = " | ".join(activation_list) + f" = 0x{activation_mask:02X}"

        event_sub = policy.get("event_subscribe", [])
        event_sub_mask = event_subscribe_to_bitmask(event_sub, events)

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
            f"/* activation: {act_comment} */",
            f"#define {prefix}ACTIVATION_MASK   0x{activation_mask:02X}U",
            f"#define {prefix}EVENT_SUBSCRIBE   0x{event_sub_mask:08X}U",
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


def generate_policy_bin(policy: dict, events_map: dict) -> bytes:
    """Pack fuse_policy_t as 8 x little-endian uint32_t (32 bytes)."""
    caps = policy["capabilities"]
    bitmask = caps_to_bitmask(caps)
    activation_list = policy.get("activation", ["INTERVAL"])
    activation_mask = activation_to_bitmask(activation_list)
    event_sub = policy.get("event_subscribe", [])
    event_sub_mask = event_subscribe_to_bitmask(event_sub, events_map)
    return struct.pack(
        "<8I",
        bitmask,
        policy["memory_pages_max"],
        policy["stack_size"],
        policy["heap_size"],
        policy["cpu_quota_us"],
        policy["step_interval_us"],
        activation_mask,
        event_sub_mask,
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
    events_map = config["application"].get("events", {})
    modules = config.get("modules", [])
    for mod in modules:
        bin_name = f"{mod['name']}_policy.bin"
        bin_path = output_dir / bin_name
        bin_path.write_bytes(generate_policy_bin(mod["policy"], events_map))
        print(f"Generated: {bin_path}")

    print(f"\nSuccess: {2 + len(modules)} files written to {output_dir}")


if __name__ == "__main__":
    main()
