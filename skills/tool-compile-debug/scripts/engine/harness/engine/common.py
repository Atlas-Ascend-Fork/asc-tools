# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any, Iterable

PROJECT_ROOT = Path(__file__).resolve().parents[2]
# Short diagnostic commands have a finite default; callers may supply a budget.
DEFAULT_COMMAND_TIMEOUT_SECONDS = 30


def external_data_root() -> Path:
    """Locate generated data outside source and installation trees, without writes."""
    path = Path(
        os.environ.get(
            "ACD_DATA_ROOT",
            str(Path(tempfile.gettempdir()) / "ascendc-compile-doctor-data"),
        )
    ).resolve()
    if path == PROJECT_ROOT or path.is_relative_to(PROJECT_ROOT):
        raise ValueError(
            "ACD_DATA_ROOT must be outside the repository or installed engine"
        )
    return path


def force_utf8_stdio() -> None:
    """Configure UTF-8 stdout and stderr so structured diagnostics remain lossless on every supported console."""
    import sys

    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
        except Exception:
            pass  # 非 TextIO（重定向到管道/已关闭）时静默——打印层故障不该致命


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # A killed writer must not turn a complete prior receipt into truncated JSON.
    fd, temporary = tempfile.mkstemp(prefix="." + path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def error_payload(
    stage: str, exc: Exception | str, error_type: str | None = None, **details: Any
) -> dict[str, Any]:
    message = str(exc)
    payload: dict[str, Any] = {
        "type": error_type
        or (exc.__class__.__name__ if isinstance(exc, Exception) else "RuntimeError"),
        "message": message,
        "stage": stage,
        "summary": f"{error_type or (exc.__class__.__name__ if isinstance(exc, Exception) else 'RuntimeError')}: {message}",
    }
    if details:
        payload["details"] = details
    return {"error": payload}


def load_json(path: str | Path | None, default: Any = None) -> Any:
    if not path:
        return default
    p = Path(path)
    if not p.exists():
        return default
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return default


def load_yaml(path: Path, default: Any = None) -> Any:
    try:
        import yaml
    except Exception:
        return default
    if not path.exists():
        return default
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8")) or default
    except Exception:
        return default


class KnowledgeBaseIntegrityError(ValueError):
    """The installed knowledge-base files do not match their integrity manifest."""


def _load_api_signature_shards(shard_dir: Path) -> list[dict[str, Any]]:
    """Read all declared shards, rejecting partial or modified installations."""
    try:
        import yaml
    except ImportError as exc:
        raise KnowledgeBaseIntegrityError(
            "API signature shards require PyYAML"
        ) from exc

    manifest_path = shard_dir / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as exc:
        raise KnowledgeBaseIntegrityError(
            f"API signature manifest is missing or unreadable: {manifest_path}"
        ) from exc
    if (
        not isinstance(manifest, dict)
        or manifest.get("schema_version") != "api-signatures-shards-v1"
        or type(manifest.get("entry_count")) is not int
        or manifest["entry_count"] <= 0
        or not isinstance(manifest.get("parts"), list)
        or not manifest["parts"]
    ):
        raise KnowledgeBaseIntegrityError("Invalid API signature manifest")

    parts = manifest["parts"]
    names: list[str] = []
    for index, part in enumerate(parts, start=1):
        expected_name = f"part-{index:04d}.yaml"
        if (
            not isinstance(part, dict)
            or part.get("name") != expected_name
            or type(part.get("entry_count")) is not int
            or part["entry_count"] <= 0
            or not isinstance(part.get("sha256"), str)
            or re.fullmatch(r"[0-9a-f]{64}", part["sha256"]) is None
        ):
            raise KnowledgeBaseIntegrityError(
                f"Invalid API signature manifest part: {expected_name}"
            )
        names.append(expected_name)

    expected_files = {*names, "manifest.json"}
    try:
        actual_files = {path.name for path in shard_dir.iterdir()}
    except OSError as exc:
        raise KnowledgeBaseIntegrityError(
            f"Cannot read API signature shard directory: {shard_dir}"
        ) from exc
    if actual_files != expected_files:
        raise KnowledgeBaseIntegrityError(
            "API signature shard file set mismatch: "
            f"missing={sorted(expected_files - actual_files)}, "
            f"unexpected={sorted(actual_files - expected_files)}"
        )

    merged: list[dict[str, Any]] = []
    for part in parts:
        path = shard_dir / part["name"]
        if path.is_symlink() or path.resolve().parent != shard_dir.resolve():
            raise KnowledgeBaseIntegrityError(
                f"API signature shard must be package-local: {part['name']}"
            )
        try:
            content = path.read_bytes()
            if hashlib.sha256(content).hexdigest() != part["sha256"]:
                raise KnowledgeBaseIntegrityError(
                    f"API signature shard checksum mismatch: {part['name']}"
                )
            entries = yaml.safe_load(content.decode("utf-8"))
        except (OSError, UnicodeError, yaml.YAMLError) as exc:
            raise KnowledgeBaseIntegrityError(
                f"Unreadable API signature shard: {part['name']}"
            ) from exc
        if (
            not isinstance(entries, list)
            or len(entries) != part["entry_count"]
            or any(
                not isinstance(entry, dict)
                or not isinstance(entry.get("name"), str)
                or not entry["name"]
                for entry in entries
            )
        ):
            raise KnowledgeBaseIntegrityError(
                f"Invalid API signature entries: {part['name']}"
            )
        merged.extend(entries)
    if len(merged) != manifest["entry_count"]:
        raise KnowledgeBaseIntegrityError("API signature total entry count mismatch")
    return merged


def load_api_signature_kb(configs_dir: Path) -> list[Any]:
    """Load the API signature knowledge base from ``configs_dir``.

    单文件 ``api_signatures.yaml`` 优先，并保留缺失或解析失败时返回空列表的兼容行为。
    分片形态由 ``api_signatures/manifest.json`` 声明完整有序文件集、条目数和校验值；
    不完整或损坏时抛出 ``KnowledgeBaseIntegrityError``，不能静默使用部分知识。
    """
    single = configs_dir / "api_signatures.yaml"
    if single.is_file():
        entries = load_yaml(single, default=[])
        return entries if isinstance(entries, list) else []
    shard_dir = configs_dir / "api_signatures"
    return _load_api_signature_shards(shard_dir) if shard_dir.is_dir() else []


def which(name: str) -> str | None:
    found = shutil.which(name)
    if found:
        return found
    candidates = [
        Path("/usr/local/Ascend/ascend-toolkit/latest/bin") / name,
        Path("/usr/local/Ascend/ascend-toolkit/latest/aarch64-linux/bin") / name,
        Path("/usr/local/Ascend/ascend-toolkit/latest/tools") / name,
        Path("/usr/local/Ascend/ascend-toolkit/latest/compiler/ccec_compiler/bin")
        / name,
    ]
    for candidate in candidates:
        if candidate.exists() and os.access(candidate, os.X_OK):
            return str(candidate)
    return None


def run_command(
    argv: list[str],
    cwd: Path | None = None,
    timeout: int = DEFAULT_COMMAND_TIMEOUT_SECONDS,
) -> dict[str, Any]:
    def _text_output(value: Any) -> str:
        if value is None:
            return ""
        if isinstance(value, bytes):
            return value.decode("utf-8", errors="replace")
        return str(value)

    try:
        proc = subprocess.run(
            argv,
            cwd=str(cwd) if cwd else None,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
        return {"returncode": proc.returncode, "output": proc.stdout}
    except FileNotFoundError as exc:
        return {"returncode": 127, "output": str(exc), "error": str(exc)}
    except OSError as exc:
        return {"returncode": 126, "output": str(exc), "error": str(exc)}
    except subprocess.TimeoutExpired as exc:
        return {
            "returncode": 124,
            "output": _text_output(exc.stdout or exc.output),
            "error": "timeout",
        }


def first_existing(paths: Iterable[str | Path]) -> Path | None:
    for path in paths:
        if not path:
            continue
        p = Path(path).expanduser()
        if p.exists():
            return p
    return None


def relpath(path: str | Path, base: str | Path) -> str:
    try:
        return str(Path(path).resolve().relative_to(Path(base).resolve()))
    except Exception:
        return str(path)


_ECHO_MAX_TREE_ENTRIES = 10_000
_ECHO_MAX_SOURCE_BYTES = 1_000_000
_ECHO_SOURCE_SUFFIXES = frozenset(
    {".asc", ".cce", ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh"}
)


def _echo_visible_code(text: str) -> str:
    # Mask only to test relevance; the actual line comparison never changes
    # comments, literals, internal whitespace or punctuation.
    pattern = r'//[^\n]*|/\*[\s\S]*?(?:\*/|\Z)|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\''
    return re.sub(
        pattern, lambda m: "".join("\n" if c == "\n" else " " for c in m.group()), text
    )


def _source_echo_has_anchor(echo: str, message: str, column: int | None = None) -> bool:
    if not isinstance(echo, str) or not isinstance(message, str) or not echo.strip():
        return False
    if any(c in echo for c in "\r\n\t") or re.search(r'\b(?:u8|u|U|L)?R"', echo):
        return False
    visible = _echo_visible_code(echo)
    # A quoted diagnostic identifier must occur in an expression, not just a
    # generic brace/template/return line or an identifier hidden in a literal.
    if not re.search(r"[=(\[.]|->", visible):
        return False
    symbols = re.findall(r"['‘]([A-Za-z_]\w*(?:::\w+)*)['’]", message)
    for symbol in symbols:
        for match in re.finditer(
            r"(?<![\w:])(?:[A-Za-z_]\w*::)*" + re.escape(symbol) + r"\b", visible
        ):
            if column is None or (
                isinstance(column, int) and match.start() <= column - 1 < match.end()
            ):
                return True
    return False


def _source_echo_candidate(
    norm: str,
    project: Path,
    line: int | None,
    echo: str | None,
    message: str | None,
    evidence: dict[str, Any] | None,
) -> Path | None:
    if (
        not isinstance(line, int)
        or isinstance(line, bool)
        or line <= 0
        or not _source_echo_has_anchor(echo, message)
    ):
        return None
    filename = norm.rsplit("/", 1)[-1]
    if Path(filename).suffix.lower() not in _ECHO_SOURCE_SUFFIXES or any(
        c in norm for c in "<>\x00"
    ):
        return None
    if re.search(
        r"(?:^|/)(?:usr/local/Ascend|opt/[Aa]scend)(?:/|$)|/(?:tikcpp/tikcfw|asc/(?:include|impl))/",
        norm,
    ):
        return None
    try:
        raw_path = Path(norm)
        if "$" not in norm and any(
            p.is_symlink() or (hasattr(p, "is_junction") and p.is_junction())
            for p in (raw_path, *raw_path.parents)
        ):
            return None
        pending, visited, candidates = [(project, 0)], set(), set()
        entries = 0
        while pending:
            directory, depth = pending.pop()
            if depth > 64:
                return None
            real = directory.resolve(strict=True)
            if not real.is_relative_to(project) or real in visited:
                continue
            visited.add(real)
            # scandir failures are fatal to uniqueness; no ignored directory
            # lists or rglob permission-error suppression on this fallback.
            with os.scandir(real) as scan:
                for entry in scan:
                    entries += 1
                    if entries > _ECHO_MAX_TREE_ENTRIES:
                        return None
                    path = Path(entry.path).resolve(strict=True)
                    if not path.is_relative_to(project):
                        continue  # External aliases are outside the allowed scope.
                    if path.is_dir():
                        pending.append((path, depth + 1))
                    elif entry.name == filename and path.is_file():
                        candidates.add(
                            path
                        )  # Exact filename case; deduplicate aliases.
        matches = []
        for path in sorted(candidates):
            with path.open("rb") as stream:
                data = stream.read(_ECHO_MAX_SOURCE_BYTES + 1)
            if len(data) > _ECHO_MAX_SOURCE_BYTES or path.resolve(strict=True) != path:
                return None
            text = data.decode("utf-8").replace("\r\n", "\n").replace("\r", "\n")
            # Line splicing precedes C++ comment recognition. This fallback
            # deliberately declines such files instead of approximating a
            # preprocessor (including trigraph backslashes and spaced splices).
            if re.search(r"(?:\\|\?\?/)[ \t]*\n", text) or re.search(
                r'\b(?:u8|u|U|L)?R"', text
            ):
                return None
            visible_text = _echo_visible_code(text)
            if re.search(r"^\s*(?:#|%:)\s*(?:line\b|\d+\b)", visible_text, re.M):
                return None  # Includes comments in directives and the # digraph.
            # str.splitlines also splits VT/FF/NEL/Unicode separators, which
            # are not physical compiler line boundaries and can change after
            # comment masking. Both views must use the same physical LF lines.
            lines = text.split("\n")
            if line > len(lines) or lines[line - 1].strip(" \t") != echo.strip(" \t"):
                continue
            visible_line = visible_text.split("\n")[line - 1]
            if not _source_echo_has_anchor(visible_line, message):
                continue  # The apparent expression may be in a multiline comment.
            matches.append((path, data, text))
            if len(matches) > 1:
                return None
        if len(matches) == 1:
            path, data, text = matches[0]
            if evidence is not None:
                evidence.update(
                    source_sha256=hashlib.sha256(data).hexdigest(), source_text=text
                )
            return path
    except (OSError, RuntimeError, ValueError, UnicodeError):
        return None
    return None


def resolve_project_source(
    raw: str | Path,
    project: Path,
    *,
    reanchor: bool = False,
    external_roots: Iterable[str | Path] = (),
    source_line: int | None = None,
    source_echo: str | None = None,
    diagnostic_message: str | None = None,
    source_evidence: dict[str, Any] | None = None,
) -> Path | None:
    """Resolve a file owned by the explicit project, never a lexical escape.

    Relative paths may name a file below the project or include the explicit
    project directory name. Conflicting existing interpretations are ambiguous.
    Historical roots may be replaced using a unique directory suffix. A bare
    filename elsewhere in the tree cannot establish historical source identity.
    External declarations remain log evidence, but are not project-owned files.
    """
    project = Path(project).resolve()
    norm = str(raw).replace("\\", "/").strip()
    if not norm:
        return None

    def owned(candidate: Path) -> Path | None:
        try:
            resolved = candidate.resolve(strict=True)
            return (
                resolved
                if resolved.is_file() and resolved.is_relative_to(project)
                else None
            )
        except (OSError, RuntimeError, ValueError):
            return None

    path = Path(norm)
    candidate = path if path.is_absolute() else project / path
    direct = owned(candidate) if "$" not in norm else None
    parts = [part for part in norm.split("/") if part and part != "."]
    if (
        not path.is_absolute()
        and len(parts) > 1
        and parts[0] == project.name
        and ".." not in parts
        and "$" not in norm
    ):
        # Logs produced from the project's parent commonly retain this prefix.
        # This is an explicit relative coordinate, not a historical suffix search.
        qualified_candidate = project.joinpath(*parts[1:])
        qualified = owned(qualified_candidate)
        try:
            direct_present = candidate.exists() or candidate.is_symlink()
            qualified_present = (
                qualified_candidate.exists() or qualified_candidate.is_symlink()
            )
        except OSError:
            return None
        # An existing external link is a conflicting interpretation, not a
        # missing path that permits silently selecting the other local file.
        if (direct_present and direct is None) or (
            qualified_present and qualified is None
        ):
            return None
        if qualified is not None:
            if direct is not None and direct != qualified:
                return None
            return qualified
    if direct is not None:
        return direct
    # A known dependency root is external diagnostic evidence, even if a local
    # wrapper happens to have the same directory suffix. Do not guess SDK roots.
    for external in external_roots:
        if not external:
            continue
        # Linux log coordinates must retain their declared root even when an
        # offline Windows project and the current directory use different drives.
        # Native Path.resolve() would otherwise attach different drive letters.
        literal_root = PurePosixPath(str(external).replace("\\", "/"))
        literal_source = PurePosixPath(norm)
        if (
            literal_source.is_absolute()
            and literal_root.is_absolute()
            and literal_source.is_relative_to(literal_root)
        ):
            return None
        try:
            if candidate.resolve().is_relative_to(Path(external).resolve()):
                return None
        except (OSError, RuntimeError, ValueError):
            continue
    if not parts or any(character in parts[-1] for character in "*?[]"):
        return None
    # An in-project link that resolves outside, or a parent traversal that did
    # not resolve inside, must not be retried as some unrelated suffix.
    try:
        lexical_inside = candidate.absolute().is_relative_to(project)
    except (OSError, ValueError):
        lexical_inside = False
    if not reanchor or ".." in parts or (lexical_inside and "$" not in norm):
        return None
    # Match directory + filename (or a longer suffix), and require uniqueness
    # across the whole current tree rather than selecting the first basename.
    # The explicit project directory is part of this inferred identity too:
    # root-level files otherwise lose their only directory component. Never
    # include the current machine's absolute parent directories in the match.
    try:
        files = [
            (file, [project.name, *file.relative_to(project).as_posix().split("/")])
            for file in project.rglob(parts[-1])
            if file.is_file() and owned(file) is not None
        ]
    except (OSError, ValueError, IndexError):
        return None
    for width in range(len(parts), 1, -1):
        suffix = parts[-width:]
        matches = {
            owned(file) for file, relative in files if relative[-width:] == suffix
        }
        matches.discard(None)
        if matches:
            return next(iter(matches)) if len(matches) == 1 else None
    return _source_echo_candidate(
        norm, project, source_line, source_echo, diagnostic_message, source_evidence
    )


def classify_source_path(path: str) -> str:
    lower = path.lower()
    if "op_host" in lower or lower.endswith("_host.cpp"):
        return "host"
    if "op_kernel" in lower or lower.endswith(".asc") or "kernel" in lower:
        return "kernel"
    return "unknown"


def guess_phase_from_file(path: str, fallback: str = "unknown_compile") -> str:
    kind = classify_source_path(path)
    if kind == "host":
        return "host_compile"
    if kind == "kernel":
        return "kernel_compile"
    return fallback


def redact_path(value: str) -> str:
    home = str(Path.home())
    project_root = str(PROJECT_ROOT)
    redacted = value.replace(home, "$HOME")
    redacted = redacted.replace(project_root, "$PROJECT_ROOT")
    return redacted


def line_number_for(text: str, pattern: str, regex: bool = False) -> int | None:
    for i, line in enumerate(text.splitlines(), 1):
        if re.search(pattern, line) if regex else pattern in line:
            return i
    return None


def file_snippet(path: Path, max_bytes: int = 200_000) -> str:
    try:
        data = path.read_bytes()
    except Exception:
        return ""
    if len(data) > max_bytes:
        data = data[:max_bytes]
    return data.decode("utf-8", errors="replace")
