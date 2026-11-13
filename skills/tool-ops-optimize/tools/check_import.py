#!/usr/bin/env python3
# Copyright (c) 2026 AProf contributors.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
"""Check immutable import payloads with only Python's standard library."""

import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys

_HELPER = (
    Path(__file__).resolve().parents[1]
    / "runtime/src/aprof_runtime/documentation_sources.py"
)
_SPEC = importlib.util.spec_from_file_location("aprof_documentation_sources", _HELPER)
_DOCS = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_DOCS)

FROZEN = re.compile(
    r"^(?:sources/|skills/[^/]+/(?:sources/|graph/|knowledge/)|"
    r"workflows/(?:migration/|references/[^/]+/)|"
    r"runtime/src/aprof_runtime/(?:_diagnosis_v1|_knowledge_v1)\.py$|schemas/|skills/[^/]+/(?:LICENSE|NOTICE\.md)$)"
)


def verify(root):
    root = Path(root).resolve()
    manifest = json.loads(
        (root / "tools/frozen-files.json").read_text(encoding="utf-8")
    )
    expected = manifest["files"]
    adapted = _DOCS.adaptations(root)
    archived = _DOCS.source_archives(root)
    if not set(adapted) <= set(expected):
        raise ValueError("adapted document is not a declared upstream source")
    if not set(archived) <= set(expected):
        raise ValueError("archived header is not a declared upstream source")
    for name, entry in archived.items():
        if entry["sha256"] != expected[name]:
            raise ValueError("source archive differs from frozen checksum: " + name)
    physical = (set(expected) - set(archived)) | {
        entry["path"] for entry in archived.values()
    }
    actual = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file()
        and FROZEN.match(path.relative_to(root).as_posix())
        and "__pycache__" not in path.parts
        and path.suffix not in {".pyc", ".pyo"}
    }
    if actual != physical:
        raise ValueError(
            "frozen inventory differs: " + ", ".join(sorted(actual ^ physical)[:8])
        )
    for name, checksum in expected.items():
        path = root / name
        if not path.resolve().is_relative_to(root) or path.is_symlink():
            raise ValueError("unsafe frozen path: " + name)
        path = _DOCS.original_path(root, name, entries=adapted, archives=archived)
        if hashlib.sha256(path.read_bytes()).hexdigest() != checksum:
            raise ValueError("frozen bytes changed: " + name)
    registry = json.loads((root / "skills/registry.json").read_text(encoding="utf-8"))
    for row in registry["capabilities"]["core"] + registry["support_skills"]:
        source = root / row["source"] / "SKILL.md"
        if not source.resolve().is_relative_to(root) or not source.is_file():
            raise ValueError("missing skill: " + row["name"])
        if not re.search(
            r"^name:\s*" + re.escape(row["name"]) + r"\s*$",
            source.read_text(encoding="utf-8"),
            re.M,
        ):
            raise ValueError("skill name differs: " + row["name"])
    for source in registry["workflow_plugin"]["agents"]:
        if not (root / source).is_file():
            raise ValueError("missing workflow agent: " + source)
    print(
        "AProf import verified: {} frozen files, {} core roles.".format(
            len(expected), len(registry["capabilities"]["core"])
        )
    )


if __name__ == "__main__":
    try:
        verify(Path(__file__).resolve().parents[1])
    except (OSError, ValueError, KeyError) as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)
