#!/usr/bin/env python3
"""
policy_to_bin.py - Convert a FUSE policy JSON file to a binary file matching
the fuse_policy_t C struct layout.

fuse_policy_t layout (24 bytes, all little-endian uint32_t):
    uint32_t capabilities;      offset  0
    uint32_t memory_pages_max;  offset  4
    uint32_t stack_size;        offset  8
    uint32_t heap_size;         offset 12
    uint32_t cpu_quota_us;      offset 16
    uint32_t step_interval_us;  offset 20

Usage:
    python3 policy_to_bin.py --input <json_file> --output <bin_file>
"""

import argparse
import json
import struct
import sys


POLICY_FIELDS = [
    "capabilities",
    "memory_pages_max",
    "stack_size",
    "heap_size",
    "cpu_quota_us",
    "step_interval_us",
]

PACK_FORMAT = "<6I"
EXPECTED_SIZE = 24


def parse_args():
    parser = argparse.ArgumentParser(
        description="Convert a FUSE policy JSON file to a packed binary matching fuse_policy_t."
    )
    parser.add_argument(
        "--input",
        required=True,
        metavar="<json_file>",
        help="Path to the input policy JSON file.",
    )
    parser.add_argument(
        "--output",
        required=True,
        metavar="<bin_file>",
        help="Path to the output binary file.",
    )
    return parser.parse_args()


def main():
    args = parse_args()

    try:
        with open(args.input, "r") as f:
            policy = json.load(f)
    except FileNotFoundError:
        print(f"Error: input file not found: {args.input}", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"Error: failed to parse JSON from {args.input}: {e}", file=sys.stderr)
        sys.exit(1)

    values = []
    for field in POLICY_FIELDS:
        if field not in policy:
            print(f"Error: missing required field '{field}' in {args.input}", file=sys.stderr)
            sys.exit(1)
        raw = policy[field]
        if not isinstance(raw, int):
            print(
                f"Error: field '{field}' must be an integer, got {type(raw).__name__}",
                file=sys.stderr,
            )
            sys.exit(1)
        if raw < 0 or raw > 0xFFFFFFFF:
            print(
                f"Error: field '{field}' value {raw} is out of uint32_t range [0, 4294967295]",
                file=sys.stderr,
            )
            sys.exit(1)
        values.append(raw)

    packed = struct.pack(PACK_FORMAT, *values)
    assert len(packed) == EXPECTED_SIZE

    try:
        with open(args.output, "wb") as f:
            f.write(packed)
    except OSError as e:
        print(f"Error: failed to write output file {args.output}: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"Written policy binary: {args.output} ({EXPECTED_SIZE} bytes)")


if __name__ == "__main__":
    main()
