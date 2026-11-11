# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Bounded project reads shared by context collection, rules and localization."""

from __future__ import annotations

import hashlib
import re
from pathlib import Path

from harness.engine.common import resolve_project_source


class SourceReader:
    """One query owns one cache. Interpretations never outlive their context."""

    def __init__(
        self,
        root: Path,
        *,
        max_files: int = 64,
        max_bytes: int = 1048576,
        additional_roots=(),
    ):
        self.root = root.resolve()
        self.allowed_roots = [self.root]
        for extra in additional_roots:
            extra = Path(extra).resolve()
            if not any(extra.is_relative_to(r) for r in self.allowed_roots):
                self.allowed_roots.append(extra)
        self.max_files = max_files
        self.max_bytes = max_bytes
        self.texts: dict[str, str] = {}
        self.raw_texts: dict[str, str] = {}
        self.facts: dict[str, dict] = {}
        self.gaps: list[dict] = []
        self.bytes_read = 0
        self.cache_hits = 0

    def within_scope(self, path: str | Path) -> bool:
        candidate = Path(path)
        candidate = (
            candidate if candidate.is_absolute() else self.root / candidate
        ).resolve()
        return any(candidate.is_relative_to(root) for root in self.allowed_roots)

    def resolve(self, path: str | Path) -> Path | None:
        candidate = Path(path)
        candidate = candidate if candidate.is_absolute() else self.root / candidate
        for root in self.allowed_roots:
            resolved = resolve_project_source(candidate, root)
            if resolved is not None:
                return resolved
        return None

    def key(self, path: Path) -> str:
        path = path.resolve()
        return (
            path.relative_to(self.root).as_posix()
            if path.is_relative_to(self.root)
            else path.as_posix()
        )

    def read(self, path: str | Path) -> str | None:
        resolved = self.resolve(path)
        if resolved is None:
            self.gaps.append({"path": str(path), "reason": "missing_or_outside_scope"})
            return None
        rel = self.key(resolved)
        if rel in self.texts:
            self.cache_hits += 1
            return self.texts[rel]
        try:
            size = resolved.stat().st_size
            if (
                len(self.texts) >= self.max_files
                or size > self.max_bytes - self.bytes_read
            ):
                self.gaps.append({"path": rel, "reason": "read_budget"})
                return None
            with resolved.open("rb") as stream:
                raw = stream.read(self.max_bytes - self.bytes_read + 1)
            if len(raw) > self.max_bytes - self.bytes_read:
                self.gaps.append({"path": rel, "reason": "changed_or_read_budget"})
                return None
        except OSError as exc:
            self.gaps.append({"path": rel, "reason": type(exc).__name__})
            return None
        self.bytes_read += len(raw)
        # Match read_text's universal-newline semantics; retain raw bytes for identity.
        self.raw_texts[rel] = raw.decode("utf-8", errors="replace")
        text = self.raw_texts[rel].replace("\r\n", "\n").replace("\r", "\n")
        self.texts[rel] = text
        self.facts[rel] = {"sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)}
        return text

    def read_raw(self, path: str | Path) -> str | None:
        """Same budget/cache and byte identity, without rewriting quoted CR/LF."""
        if self.read(path) is None:
            return None
        resolved = self.resolve(path)
        return self.raw_texts.get(self.key(resolved)) if resolved else None

    def include_closure(
        self, source: str | Path, *, include_dirs=(), max_depth: int = 8
    ):
        """Read literal project includes; macros and unresolved headers remain gaps.

        This is a lexical candidate graph, not a preprocessor visibility proof.
        """
        queue = [(source, 0)]
        visited = set()
        edges = []
        while queue:
            current, depth = queue.pop(0)
            path = self.resolve(current)
            if path is None or path in visited:
                continue
            visited.add(path)
            text = self.read(path)
            if text is None:
                continue
            for line, match_line in enumerate(text.splitlines(), 1):
                match = re.match(r'\s*#\s*include\s*([<"])([^">]+)[">]', match_line)
                if not match:
                    if re.match(r"\s*#\s*include\b", match_line):
                        self.gaps.append(
                            {"path": str(path), "line": line, "reason": "macro_include"}
                        )
                    continue
                name = match.group(2)
                dirs = (
                    ([path.parent] if match.group(1) == '"' else [])
                    + list(include_dirs)
                    + [self.root]
                )
                candidates = []
                for directory in dirs:
                    candidate = self.resolve(Path(directory) / name)
                    if candidate is not None and candidate not in candidates:
                        candidates.append(candidate)
                edges.append(
                    {
                        "from": self.key(path),
                        "line": line,
                        "include": name,
                        "candidates": [self.key(p) for p in candidates],
                        "visibility": "lexical_only",
                    }
                )
                if not candidates:
                    self.gaps.append(
                        {"path": name, "reason": "include_not_resolved_in_project"}
                    )
                elif depth >= max_depth:
                    self.gaps.append({"path": name, "reason": "include_depth"})
                else:
                    queue.extend((p, depth + 1) for p in candidates)
        return edges

    def summary(self):
        return {
            "files_read": len(self.texts),
            "bytes_read": self.bytes_read,
            "cache_hits": self.cache_hits,
            "gaps": self.gaps,
            "scope": str(self.root),
            "allowed_roots": [str(r) for r in self.allowed_roots],
            "files": self.facts,
        }
