#!/usr/bin/env python3
"""Print the Build ID from a Nintendo Switch NSO0 executable header.

Use only on an executable extracted from your own game dump. This script reads
only the NSO header; it does not decrypt XCI/NSP files or handle keys.
"""
from __future__ import annotations

import argparse
import pathlib
import sys

NSO_MAGIC = b"NSO0"
BUILD_ID_OFFSET = 0x40
BUILD_ID_SIZE = 0x20
HEADER_MIN_SIZE = BUILD_ID_OFFSET + BUILD_ID_SIZE


def read_build_id(path: pathlib.Path) -> str:
    with path.open("rb") as stream:
        header = stream.read(HEADER_MIN_SIZE)
    if len(header) < HEADER_MIN_SIZE:
        raise ValueError(f"file is too short for an NSO Build ID ({len(header)} bytes)")
    if header[:4] != NSO_MAGIC:
        raise ValueError("not an NSO0 executable; expected the raw ExeFS 'main' file")
    return header[BUILD_ID_OFFSET:HEADER_MIN_SIZE].hex().upper()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("main_nso", type=pathlib.Path, help="path to the extracted ExeFS main NSO")
    args = parser.parse_args()
    try:
        print(read_build_id(args.main_nso))
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
