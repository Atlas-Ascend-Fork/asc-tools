#!/usr/bin/env python3
# -*- coding: UTF-8 -*-
# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Check or rebuild the API shard manifest after an intentional source edit.

用途：维护源码时校验知识分片，按显式预期数量更新完整性清单。
Usage: python3 update_kb_manifest.py --check
       python3 update_kb_manifest.py --write --expected-parts N --expected-entries N

The default operation is read-only. Writing replaces only manifest.json; it
does not change YAML, run a compiler, contact a service, or repair an install.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
import tempfile
from pathlib import Path

import yaml

DEFAULT_DIRECTORY = (
    Path(__file__).resolve().parent / "engine/harness/knowledge/api_signatures"
)
SHARD_MAX_BYTES = 450 * 1024
SCHEMA_VERSION = "api-signatures-shards-v1"
ENTRY_FIELDS = ("name", "signature", "header", "kind", "api_level", "category", "brief")


class UniqueKeyLoader(yaml.SafeLoader):
    """Reject duplicate mapping keys instead of silently dropping a value."""

    def construct_mapping(self, node, deep=False):
        self.flatten_mapping(node)
        keys = set()
        for key_node, _ in node.value:
            key = self.construct_object(key_node, deep=deep)
            if not isinstance(key, str) or key in keys:
                raise ValueError(f"duplicate or non-string YAML key: {key!r}")
            keys.add(key)
        return super().construct_mapping(node, deep=deep)


def build_manifest(directory: Path, expected_parts: int, expected_entries: int) -> dict:
    """Validate the complete declared file set before deriving checksums."""
    if expected_parts < 1 or expected_entries < 1:
        raise ValueError("expected part and entry counts must be positive")
    if directory.is_symlink():
        raise ValueError("the shard directory must not be a symbolic link")
    names = [f"part-{index:04d}.yaml" for index in range(1, expected_parts + 1)]
    actual = {path.name for path in directory.iterdir()}
    expected = {*names, "manifest.json"}
    # A new source tree may not have its first manifest yet.
    if actual - expected or set(names) - actual:
        raise ValueError(
            f"shard file set mismatch: missing={sorted(set(names) - actual)}, "
            f"unexpected={sorted(actual - expected)}"
        )
    parts = []
    for name in names:
        path = directory / name
        if path.is_symlink() or path.resolve().parent != directory.resolve():
            raise ValueError(f"shard must be a local regular file: {name}")
        content = path.read_bytes()
        if len(content) > SHARD_MAX_BYTES:
            raise ValueError(f"{name} exceeds the {SHARD_MAX_BYTES}-byte shard budget")
        text = content.decode("utf-8")
        if "\r" in text or text.startswith("\ufeff"):
            raise ValueError(f"{name} must use UTF-8 without BOM and LF line endings")
        entries = yaml.load(text, Loader=UniqueKeyLoader)
        if not isinstance(entries, list) or not entries:
            raise ValueError(f"{name} must contain a nonempty list")
        for index, entry in enumerate(entries, start=1):
            if not isinstance(entry, dict):
                raise ValueError(f"{name} entry {index} must be a mapping")
            for field in ENTRY_FIELDS:
                value = entry.get(field)
                if not isinstance(value, str) or (
                    field != "brief" and not value.strip()
                ):
                    raise ValueError(
                        f"{name} entry {index} has an invalid {field} field"
                    )
        parts.append(
            {
                "name": name,
                "entry_count": len(entries),
                "sha256": hashlib.sha256(content).hexdigest(),
            }
        )
    total = sum(part["entry_count"] for part in parts)
    if total != expected_entries:
        raise ValueError(
            f"entry count mismatch: expected {expected_entries}, found {total}"
        )
    return {"schema_version": SCHEMA_VERSION, "entry_count": total, "parts": parts}


def check_manifest(directory: Path) -> dict:
    """Check against the recorded inventory without accepting a new baseline."""
    stored = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    if (
        not isinstance(stored, dict)
        or stored.get("schema_version") != SCHEMA_VERSION
        or type(stored.get("entry_count")) is not int
        or not isinstance(stored.get("parts"), list)
    ):
        raise ValueError("invalid manifest schema")
    for part in stored["parts"]:
        if (
            not isinstance(part, dict)
            or type(part.get("entry_count")) is not int
            or not isinstance(part.get("sha256"), str)
            or re.fullmatch(r"[0-9a-f]{64}", part["sha256"]) is None
        ):
            raise ValueError("invalid manifest part")
    expected = build_manifest(directory, len(stored["parts"]), stored["entry_count"])
    if stored != expected:
        raise ValueError("manifest differs from shards; review the source change first")
    return expected


def write_manifest(directory: Path, manifest: dict) -> None:
    """Replace only the manifest, atomically, after all validation has succeeded."""
    path = directory / "manifest.json"
    if path.is_symlink():
        raise ValueError("manifest must not be a symbolic link")
    mode = stat.S_IMODE(path.stat().st_mode) if path.exists() else 0o644
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=directory, suffix=".tmp", delete=False
        ) as stream:
            temporary = Path(stream.name)
            stream.write((json.dumps(manifest, indent=2) + "\n").encode("utf-8"))
        temporary.chmod(mode)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="read-only check (default)")
    mode.add_argument(
        "--write", action="store_true", help="update after a reviewed edit"
    )
    parser.add_argument("--directory", type=Path, default=DEFAULT_DIRECTORY)
    parser.add_argument("--expected-parts", type=int)
    parser.add_argument("--expected-entries", type=int)
    args = parser.parse_args(argv)
    counts = (args.expected_parts, args.expected_entries)
    if args.write and any(count is None or count < 1 for count in counts):
        parser.error(
            "--write requires positive --expected-parts and --expected-entries"
        )
    if not args.write and any(count is not None for count in counts):
        parser.error("expected counts are only accepted with --write")
    try:
        if args.write:
            result = build_manifest(args.directory, *counts)
            write_manifest(args.directory, result)
        else:
            result = check_manifest(args.directory)
    except (OSError, UnicodeError, ValueError, yaml.YAMLError) as exc:
        print(f"Knowledge-base manifest: {exc}", file=sys.stderr)
        return 1
    verb = "updated" if args.write else "verified"
    print(
        f"Manifest {verb}: {len(result['parts'])} parts, {result['entry_count']} entries"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
