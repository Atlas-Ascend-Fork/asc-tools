# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Read-only expansion of a conservative GNU response-file subset.

No shell expansion, execution or search outside the caller's read scopes.
Nested relative @paths use the compiler command cwd, not the including directory.
"""

from __future__ import annotations

from pathlib import Path
import re

from harness.engine.source_facts import SourceReader


def _tokens(text: str, limit: int) -> list[str]:
    # GNU response syntax is not shell syntax. In particular, backslash can
    # escape quotes inside either quote style. Non-special escapes differ across
    # compiler implementations; reject them instead of guessing.
    if "\x00" in text or "\ufffd" in text or text.startswith("\ufeff"):
        raise ValueError("response_encoding")
    tokens, word = [], []
    quote, active = None, False
    i = 0
    while i < len(text):
        c = text[i]
        if c == "\\":
            i += 1
            if i == len(text) or text[i] not in " \t\r\n\\'\"":
                raise ValueError("response_syntax")
            word.append(text[i])
            active = True
        elif c in "'\"" and (quote is None or quote == c):
            quote = None if quote == c else c
            active = True
        elif c in " \t\r\n" and quote is None:
            if active:
                tokens.append("".join(word))
                if len(tokens) > limit:
                    raise ValueError("response_token_budget")
                word, active = [], False
        else:
            word.append(c)
            active = True
        i += 1
    if quote:
        raise ValueError("response_syntax")
    if active:
        tokens.append("".join(word))
    if len(tokens) > limit:
        raise ValueError("response_token_budget")
    return tokens


def expand_response_arguments(
    argv: list[str],
    cwd: Path,
    reader: SourceReader,
    *,
    max_tokens: int = 4096,
    max_depth: int = 16,
    max_response_bytes: int = 4194304,
) -> dict:
    """Preserve unresolved @tokens and their causes, with bounded total expansion."""
    if not any(x.startswith("@") for x in argv[1:]):
        return {
            "arguments": list(argv),
            "files": [],
            "status": "not_needed",
            "gaps": [],
        }
    driver = Path(argv[0]).name.lower() if argv else ""
    known = re.fullmatch(
        r"(?:[a-z0-9_]+-)*(?:gcc|g\+\+|cc|c\+\+|clang|clang\+\+|bisheng|ccec)(?:-[0-9.]+)?",
        driver,
    )
    if not known:
        return {
            "arguments": list(argv),
            "files": [],
            "status": "partial",
            "gaps": [{"reason": "response_driver_unknown", "driver": driver}],
        }
    files, gaps = {}, []
    visited_tokens = 0
    visited_bytes = 0

    def expand(tokens, stack):
        nonlocal visited_tokens, visited_bytes
        result = []
        for token in tokens:
            visited_tokens += 1
            if visited_tokens > max_tokens:
                raise ValueError("response_token_budget")
            if not token.startswith("@"):
                result.append(token)
                continue
            path = Path(token[1:])
            path = (path if path.is_absolute() else cwd / path).resolve()
            reason = None
            if not token[1:]:
                reason = "response_syntax"
            elif not reader.within_scope(path):
                reason = "response_outside_scope"
            elif path in stack:
                reason = "response_cycle"
            elif len(stack) >= max_depth:
                reason = "response_depth"
            if reason:
                gaps.append({"path": str(path), "reason": reason})
                result.append(token)
                continue
            text = reader.read_raw(path)
            if text is None:
                gaps.append({"path": str(path), "reason": "response_unreadable"})
                result.append(token)
                continue
            files[str(path)] = {"path": str(path), **reader.facts[reader.key(path)]}
            # Charge every expansion, including cache hits. A cached multi-MiB
            # single token repeated by many database rows must not multiply RAM.
            size = reader.facts[reader.key(path)]["bytes"]
            if size > max_response_bytes - visited_bytes:
                raise ValueError("response_expansion_byte_budget")
            visited_bytes += size
            try:
                nested = _tokens(text, max_tokens - visited_tokens)
                result.extend(expand(nested, (*stack, path)))
            except ValueError as exc:
                if str(exc) in {
                    "response_token_budget",
                    "response_expansion_byte_budget",
                }:
                    raise
                gaps.append({"path": str(path), "reason": str(exc)})
                result.append(token)
        return result

    try:
        expanded = [argv[0], *expand(argv[1:], ())]
    except ValueError as exc:
        # The partial expansion is not a compiler argv. Retain the original on
        # total budget exhaustion so a caller cannot accidentally execute a prefix.
        expanded = list(argv)
        if str(exc) == "response_token_budget":
            # Tokenization itself consumed the remaining allowance, even when
            # it rejected a large nested file before recursive expansion.
            visited_tokens = max_tokens + 1
        if str(exc) == "response_expansion_byte_budget":
            visited_bytes = max_response_bytes + 1
        gaps.append({"reason": str(exc)})
    return {
        "arguments": expanded,
        "files": list(files.values()),
        "status": "partial" if gaps else "complete",
        "gaps": gaps,
        "syntax": "gnu_common_subset",
        "relative_paths": "command_cwd",
        "tokens_visited": visited_tokens,
        "max_tokens": max_tokens,
        "max_depth": max_depth,
        "response_bytes_charged": visited_bytes,
        "max_response_bytes": max_response_bytes,
    }
