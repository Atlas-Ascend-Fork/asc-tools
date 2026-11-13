# Copyright (c) 2026 AProf contributors.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
"""Preserve diagnostic graph identity when source evidence is stored as text."""

from pathlib import Path

from . import _diagnosis_v1 as _compiler
from ._diagnosis_v1 import *  # noqa: F403 - preserve the legacy public exports
from .documentation_sources import original_path, source_archives

if not hasattr(_compiler, "_source_compile_region"):
    _compiler._source_compile_region = _compiler.compile_region


def compile_region(root: Path, parent_snapshot: Path) -> dict:
    """Restore verified logical header keys in the frozen source inventory."""
    root = Path(root).resolve()
    artifact = _compiler._source_compile_region(root, parent_snapshot)
    hashes = artifact["source_hashes"]
    try:
        for logical, entry in source_archives(root).items():
            physical = entry["path"]
            if physical not in hashes:
                continue
            original_path(root, logical)
            checksum = hashes.pop(physical)
            if (
                checksum.removeprefix("sha256:") != entry["sha256"]
                or hashes.get(logical, checksum) != checksum
            ):
                raise ValueError(f"source archive identity differs: {logical}")
            hashes[logical] = checksum
    except ValueError as exc:
        raise _compiler.GraphError(str(exc)) from exc
    artifact["source_hashes"] = dict(sorted(hashes.items()))
    return artifact


_compiler.compile_region = compile_region


def __getattr__(name):
    return getattr(_compiler, name)
