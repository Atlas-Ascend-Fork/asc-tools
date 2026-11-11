# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Deterministic source, diagnostic and fresh-build evidence; never executes code."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shlex
from pathlib import Path
from typing import Any

try:
    from harness.engine.build_receipts import original_compiler_arguments
    from harness.engine.build_receipts import receipt_matches
    from harness.engine.common import resolve_project_source
except ImportError:  # Standalone read-only evidence calibration.
    from build_receipts import original_compiler_arguments, receipt_matches
    from common import resolve_project_source

IGNORED_DIRS = frozenset(
    {
        ".git",
        ".venv",
        "venv",
        "__pycache__",
        "node_modules",
        "build",
        "build_out",
        "build_device",
        "build_host",
        "output",
        "out",
        "logs",
        "probe_config",
        "_CPack_Packages",
    }
)
SOURCE_SUFFIXES = frozenset(
    {".asc", ".cce", ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh"}
)
RECIPE_SUFFIXES = frozenset({".cmake", ".sh"})


def digest(value: Any) -> str:
    return hashlib.sha256(
        json.dumps(
            value, sort_keys=True, ensure_ascii=False, separators=(",", ":")
        ).encode("utf-8")
    ).hexdigest()


def source_files(root: Path):
    for directory, dirs, files in os.walk(root, followlinks=False):
        dirs[:] = sorted(d for d in dirs if d not in IGNORED_DIRS)
        for name in sorted(files):
            path = Path(directory) / name
            if (
                path.suffix.lower() in SOURCE_SUFFIXES
                or path.suffix in RECIPE_SUFFIXES
                or name
                in {"CMakeLists.txt", "CMakePresets.json", "Makefile", "makefile"}
            ):
                if resolve_project_source(path, root) is not None:
                    yield path


def source_state(root: Path) -> dict[str, Any]:
    root = root.resolve()
    files, recipes, unreadable = {}, {}, []
    for path in source_files(root):
        relative = path.relative_to(root).as_posix()
        try:
            value = hashlib.sha256(path.read_bytes()).hexdigest()
        except OSError:
            unreadable.append(relative)
            continue
        files[relative] = value
        if path.suffix in RECIPE_SUFFIXES or path.name in {
            "CMakeLists.txt",
            "CMakePresets.json",
            "Makefile",
            "makefile",
        }:
            recipes[relative] = value
    return {
        "project_id": digest(str(root)),
        "source_sha256": digest(files),
        "recipe_sha256": digest(recipes),
        "source_files": files,
        "unreadable": unreadable,
    }


def diagnostic_issues(normalized: dict, root: Path) -> list[dict]:
    root = root.resolve()
    errors = (
        normalized.get("all_errors")
        or normalized.get("errors")
        or ([normalized["primary_error"]] if normalized.get("primary_error") else [])
    )
    issues = {}
    for item in errors:
        if not isinstance(item, dict) or item.get("kind") == "warning":
            continue
        name = str(item.get("file") or "").replace("\\", "/")
        original = str(item.get("file_original") or name)
        # Raw verify logs bind only direct project paths. Diagnose may supply a
        # readable suffix candidate, whose inferred origin must remain explicit.
        resolved = (
            resolve_project_source(name, root, reanchor=False)
            if item.get("file_resolved") is not False
            else None
        )
        reanchored = bool(item.get("file_reanchored")) or bool(
            resolved is not None
            and original != name
            and original
            and resolve_project_source(original, root) is None
        )
        if resolved is not None:
            name = resolved.relative_to(root).as_posix()
        elif original:
            name = original.replace("\\", "/")
        message = re.sub(r"\s+", " ", str(item.get("message") or "")).strip()
        message = message.replace(str(root), "<PROJECT>").replace(
            root.as_posix(), "<PROJECT>"
        )
        if not message:
            continue
        # Line numbers are positions, while quoted identifiers and numeric
        # template arguments are diagnostic content and must not be erased.
        identity = {"file": name, "message": message, "category": item.get("category")}
        key = digest(identity)
        previous = issues.get(key)
        occurrence = {
            "file": name,
            "line": item.get("line"),
            "column": item.get("column"),
            "log_line": item.get("log_line"),
            "original_log_span": item.get("original_log_span"),
            "file_original": original,
            "file_reanchored": reanchored,
            "configuration_id": item.get("configuration_id"),
        }
        occurrences = [*(previous or {}).get("occurrences", []), occurrence]
        # A direct occurrence must not erase a historical inferred occurrence
        # with the same current file/message. Keep an inferred representative
        # deterministically, so log order cannot turn coverage into matched.
        if (
            previous
            and previous["file_reanchored"]
            and (not reanchored or previous["file_original"] <= original)
        ):
            previous["occurrences"] = occurrences
            continue
        issues[key] = {
            "issue_id": key,
            "occurrences": occurrences,
            **identity,
            "line": item.get("line"),
            "file_resolved": resolved is not None,
            "file_original": original,
            "file_reanchored": reanchored,
            **{
                field: item[field]
                for field in ("file_reanchor_method", "file_reanchor_source_sha256")
                if field in item
            },
        }
    return list(issues.values())


def failure_evidence(
    root: Path,
    normalized: dict,
    *,
    target: str | None = None,
    status: str = "failed",
    build_target: str | None = None,
    selected_example: str | None = None,
) -> dict:
    state = source_state(root)
    value = {
        **state,
        "status": status,
        "target": target,
        "issues": diagnostic_issues(normalized, root),
    }
    for key, explicit in (
        ("build_target", build_target),
        ("selected_example", selected_example),
    ):
        known = explicit if explicit is not None else normalized.get(key)
        if isinstance(known, str) and known.strip():
            value[key] = known
    value["evidence_id"] = digest(value)
    return value


# contract retention helpers (S4): compile success does not prove functionality
# survived. Identifiers the compiler reported at >=2 sites must still occur in
# the repaired sources; single-site disappearance stays tolerated because a
# legitimate local rewrite (did-you-mean rename, retyped call) removes exactly
# the failing use. Message shapes are compiler wording classes, not case data.
_FAILED_IDENTIFIER_PATTERNS = (
    re.compile(r"no matching function for call to '([^']+)'"),
    re.compile(r"use of undeclared identifier '([^']+)'"),
    re.compile(r"unknown type name '([^']+)'"),
    re.compile(r"'([^']+)' does not name a type"),
)
_PLAIN_IDENTIFIER = re.compile(r"^[A-Za-z_]\w*$")


def _normalize_failed_identifier(quoted: str) -> str | None:
    """Reduce a quoted symbol to the identifier whose survival we can assert.

    Handles the mainstream message forms: namespace-qualified names
    (``A::B::C`` -> ``C``) and balanced template-argument suffixes
    (``Add<int>`` -> ``Add``). Anything that is not a plain identifier afterwards
    (operators like ``operator<<`` with unbalanced angle brackets, literals,
    overloaded punctuation forms) has no word-boundary we can anchor on, so it
    is skipped rather than risk a false violation accusation.
    """
    name = _strip_template_arguments(quoted.strip())
    name = name.rsplit("::", 1)[-1].strip()
    return name if _PLAIN_IDENTIFIER.match(name) else None


def _strip_template_arguments(text: str) -> str:
    """Remove balanced ``<...>`` groups; unmatched angles stay (fails plain check)."""
    prev = None
    while prev != text:
        prev, text = text, re.sub(r"\s*<[^<>]*>", "", text)
    return text


def extract_failed_identifiers(issues: list[dict]) -> dict[str, int]:
    """Count error sites per identifier quoted in compiler failure messages.

    Non-dict entries and messages without a recognizable quoted symbol are
    ignored — a malformed prior artifact must degrade to "nothing extracted",
    never raise out of the verify pipeline.

    Event-level dedup: identical (message, file) rows collapse to one site and
    rows pointing at build products (.json/.log) are skipped; build logs are not source files.
    envelopes repeat the same compiler event and would otherwise inflate a
    single failure into the >=2-site conviction range.
    """
    counts: dict[str, int] = {}
    seen_events: set[tuple[str, str]] = set()
    for item in issues or []:
        if not isinstance(item, dict):
            continue
        message = str(item.get("message") or "")
        file_name = item.get("file")
        file_key = file_name if isinstance(file_name, str) else ""
        if str(file_key).lower().endswith((".json", ".log")):
            continue
        event = (message, file_key)
        if event in seen_events:
            continue
        seen_events.add(event)
        for pattern in _FAILED_IDENTIFIER_PATTERNS:
            for quoted in pattern.findall(message):
                name = _normalize_failed_identifier(quoted)
                if name:
                    counts[name] = counts.get(name, 0) + 1
    return counts


def identifier_retention(prior_issues: list[dict], source_root: Path | str) -> dict:
    """Multi-site failed identifiers must survive the repair in the sources.

    Survival is a word-boundary occurrence in the repaired source tree. An
    unreadable/empty source root cannot support a deletion accusation, so it
    degrades to not_applicable instead of an empty corpus (which would read as
    "everything vanished").
    """
    if not prior_issues:
        return {"status": "not_applicable"}
    counts = extract_failed_identifiers(prior_issues)
    if not counts:
        return {"status": "not_applicable"}
    joined_parts: list[str] = []
    readable = 0
    for path in source_files(Path(source_root)):
        try:
            joined_parts.append(path.read_text(encoding="utf-8", errors="replace"))
            readable += 1
        except OSError:
            continue
    if not readable:
        return {"status": "not_applicable", "reason": "source_root_not_readable"}
    joined = "\n".join(joined_parts)

    def _occurs(name: str) -> bool:
        return re.search(r"\b{}\b".format(re.escape(name)), joined) is not None

    missing = [
        {"identifier": name, "prior_error_sites": sites}
        for name, sites in sorted(counts.items())
        if sites >= 2 and not _occurs(name)
    ]
    tolerated = [
        {"identifier": name, "prior_error_sites": sites}
        for name, sites in sorted(counts.items())
        if sites < 2 and not _occurs(name)
    ]
    if missing:
        # token 消失本身无法区分「全局改名」与「删功能」：合法改名（旧名多点复用→统一换新名）
        # 与删调用位在词边界检查下同形。violated 是嫌疑不是定罪，caveat 交由调用方判读。
        return {
            "status": "violated",
            "missing": missing,
            "tolerated": tolerated,
            "caveat": "absence_of_old_token_does_not_distinguish_rename_from_removal",
        }
    return {"status": "ok", "missing": [], "tolerated": tolerated}


def _compare_build_targets(before: dict, after: dict) -> dict:
    def binding(evidence: dict) -> dict:
        return {
            key: value
            if isinstance(value := evidence.get(key), str) and value.strip()
            else None
            for key in ("build_target", "selected_example")
        }

    a, b = binding(before), binding(after)
    left, right = (
        a["build_target"] or a["selected_example"],
        b["build_target"] or b["selected_example"],
    )
    conflict = (left and right and left != right) or (
        a["selected_example"]
        and b["selected_example"]
        and a["selected_example"] != b["selected_example"]
    )
    status = (
        "conflict"
        if conflict
        else "matched"
        if left and right
        else "unknown"
        if left or right
        else "not_recorded"
    )
    return {"status": status, "before": a, "after": b}


def _issue_rows(evidence: dict | None) -> list[dict]:
    """Malformed prior rows (None / non-dict entries) degrade to skip, never raise.

    A user-supplied --previous-result file may carry issues of any shape; the
    comparison helpers must fail soft into their unavailable/not_applicable
    channels instead of escaping verify()'s error handling.
    """
    issues = (evidence or {}).get("issues")
    if not isinstance(issues, list):
        return []
    return [row for row in issues if isinstance(row, dict)]


def compare_issues(before: dict | None, after: dict) -> dict:
    if not before or before.get("project_id") != after.get("project_id"):
        return {"status": "unavailable", "reason": "no_matching_project_evidence"}
    if not before.get("target") or not after.get("target"):
        return {"status": "unavailable", "reason": "build_target_evidence_missing"}
    if before["target"] != after["target"]:
        return {"status": "unavailable", "reason": "different_build_target"}
    a = {
        iid
        for row in _issue_rows(before)
        if isinstance(iid := row.get("issue_id"), str)
    }
    b = {
        iid for row in _issue_rows(after) if isinstance(iid := row.get("issue_id"), str)
    }
    if not a and before.get("status") != "passed":
        return {"status": "unavailable", "reason": "previous_diagnostics_missing"}

    def hash_changed(key: str) -> bool | None:
        previous, current = before.get(key), after.get(key)
        if not all(
            isinstance(value, str) and value.strip() for value in (previous, current)
        ):
            return None
        return previous != current

    recipe_files_changed = hash_changed("recipe_sha256")
    recipe_arguments_changed = hash_changed("recipe_request_sha256")
    recipe_changed = (
        True
        if recipe_files_changed is True or recipe_arguments_changed is True
        else False
        if recipe_files_changed is False and recipe_arguments_changed is False
        else None
    )
    return {
        "status": "compared",
        "parent_evidence_id": before.get("evidence_id"),
        "remaining": sorted(a & b),
        "disappeared": sorted(a - b),
        "observed_new": sorted(b - a),
        "build_target_comparison": _compare_build_targets(before, after),
        "source_changed": before.get("source_sha256") != after.get("source_sha256"),
        "recipe_files_changed": recipe_files_changed,
        "recipe_arguments_changed": recipe_arguments_changed,
        "recipe_changed": recipe_changed,
        "recipe_comparison_scope": "recipe_files_and_requested_build_parameters_only",
        "note": "Diagnostic changes do not alone distinguish an exposed error from a patch regression. "
        "Repair confirmation also requires a matched build target; shared-file coverage "
        "does not establish that a different target or example was repaired. "
        "Recipe comparison covers recipe files and requested build parameters only; "
        "missing hashes leave that component unknown, and unchanged components do not "
        "establish equivalence of the complete build environment.",
    }


def command_arguments(record: dict) -> list[str] | None:
    args = record.get("arguments")
    if isinstance(args, list) and args and all(isinstance(x, str) for x in args):
        return args
    command = record.get("command")
    if not isinstance(command, str):
        return None
    try:
        args = shlex.split(command, posix=True)
    except ValueError:
        return None
    # A compilation database is not authorization to run arbitrary shell code.
    if any(x in {"&&", "||", ";", "|", ">", ">>"} for x in args):
        return None
    return args


def prior_failure_coverage(previous: dict | None, compilation: dict) -> dict:
    """Bind known source diagnostics to this build, including ordinary .cpp files.

    Kernel naming conventions alone cannot prove that a previous failing TU was
    built. Configure/recipe diagnostics do not require compiler dependency edges.
    Unresolved or suffix-inferred source origins remain unknown instead of
    implying a repair.
    """
    if not previous or previous.get("status") != "failed":
        return {"status": "not_applicable", "reason": "no_previous_failed_build"}
    rows = _issue_rows(previous)

    def _file(row: dict) -> str | None:
        # 字段类型守卫：非字符串 file（int/list/dict）会炸 Path()/集合哈希，
        # fail-soft 契约要求降级跳过而非抛异常。
        name = row.get("file")
        return name if isinstance(name, str) and name else None

    locations = {name for row in rows if (name := _file(row))}
    sources = {
        name for name in locations if Path(name).suffix.lower() in SOURCE_SUFFIXES
    }
    if not sources:
        return {"status": "not_applicable", "reason": "no_source_file_diagnostic"}
    known = set(previous.get("source_files") or {})
    explicitly_unresolved = {
        name
        for row in rows
        if row.get("file_resolved") is False and (name := _file(row))
    }
    reanchored = {
        name for row in rows if row.get("file_reanchored") and (name := _file(row))
    }
    unresolved = sorted(
        (sources - known) | (sources & (explicitly_unresolved | reanchored))
    )
    root = compilation.get("project_root")
    covered = set()
    if root:
        for record in compilation.get("records", []):
            for dep in record.get("dependencies", []):
                try:
                    covered.add(
                        Path(dep).resolve().relative_to(Path(root).resolve()).as_posix()
                    )
                except ValueError:
                    continue
    missing = sorted((sources & known) - covered)
    matched = bool(root) and not missing and not unresolved
    return {
        "status": "matched" if matched else "unconfirmed",
        "required_files": sorted(sources),
        "missing_files": missing,
        "unresolved_files": unresolved,
        "reanchored_files": sorted(sources & reanchored),
        "reason": None
        if matched
        else "previous_failure_source_identity_inferred"
        if sources & reanchored
        else "previous_failure_source_not_observed_in_current_dependencies",
    }


def command_architectures(args: list[str]) -> list[str]:
    joined = " ".join(args)
    values = set(re.findall(r"dav-(?:c)?(2201|3510)\b", joined))
    if re.search(r"dav-c220\b", joined):
        values.add("2201")

    if re.search(r"dav-c310\b", joined):
        values.add("3510")
    return sorted(values)


def _read_json(path: Path, default):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default


def _object_compilation(args: list[str]) -> bool:
    """Preprocessing and syntax-only probes do not establish object compilation."""
    return "-c" in args and not any(
        option in args for option in ("-E", "-M", "-MM", "-S", "-fsyntax-only")
    )


def _object_path(args: list[str], directory: Path, record: dict) -> Path | None:
    output = record.get("output")
    for i, arg in enumerate(args[:-1]):
        if arg == "-o":
            output = args[i + 1]
    if not output:
        return None
    path = Path(output)
    return path if path.is_absolute() else directory / path


def _dependencies(
    obj: Path, source: Path, directory: Path, args: list[str]
) -> tuple[set[Path], bool]:
    found = {source.resolve()}
    candidates = [Path(str(obj) + ".d"), obj.with_suffix(".d")]
    for index, arg in enumerate(args):
        value = (
            args[index + 1]
            if arg == "-MF" and index + 1 < len(args)
            else (arg[3:] if arg.startswith("-MF") and len(arg) > 3 else None)
        )
        if value:
            path = Path(value)
            candidates.insert(0, path if path.is_absolute() else directory / path)
    complete = False
    for dep in candidates:
        try:
            text = dep.read_text(encoding="utf-8", errors="replace").replace(
                "\\\n", " "
            )
            body = re.split(r":\s+", text, maxsplit=1)[1]
            parsed = set()
            for item in shlex.split(body):
                p = Path(item)
                parsed.add((p if p.is_absolute() else directory / p).resolve())
            found.update(parsed)
            complete = source.resolve() in parsed
            break
        except (OSError, ValueError, IndexError):
            continue
    return found, complete


def make_compilation_records(build_root: Path) -> list[dict]:
    """Read CMake ASC make recipes when CMake omits ASC from its compilation DB.

    This parses data only: no make variables, shell substitutions or recipes run.
    """
    records = []
    for directory, dirs, files in os.walk(build_root):
        dirs[:] = [
            d for d in dirs if d not in {".git", ".venv", "logs", "node_modules"}
        ]
        if "build.make" not in files:
            continue
        makefile = Path(directory) / "build.make"
        flags = makefile.with_name("flags.make")
        text = makefile.read_text(encoding="utf-8", errors="replace")
        variables = dict(
            re.findall(r"^([A-Za-z_][A-Za-z_0-9]*)[ \t]*=[ \t]*(.*)$", text, re.M)
        )
        if flags.is_file():
            variables.update(
                re.findall(
                    r"^([A-Za-z_][A-Za-z_0-9]*)[ \t]*=[ \t]*(.*)$",
                    flags.read_text(encoding="utf-8", errors="replace"),
                    re.M,
                )
            )
        cwd = Path(variables.get("CMAKE_BINARY_DIR", str(build_root))).resolve()
        for line in text.splitlines():
            if not line.startswith("\t") or " -c " not in line:
                continue
            line = re.sub(
                r"\$\(([A-Za-z_][A-Za-z_0-9]*)\)",
                lambda m, variables=variables: variables.get(m[1], m[0]),
                line.strip(),
            )
            if "$" in line or "`" in line:
                continue
            args = command_arguments({"command": line})
            if not args or "-o" not in args:
                continue
            sources = [
                a
                for a in args
                if Path(a).suffix.lower() in SOURCE_SUFFIXES and not a.startswith("-")
            ]
            if not sources:
                continue
            records.append(
                {
                    "directory": str(cwd),
                    "file": sources[-1],
                    "arguments": args,
                    "record_source": str(makefile),
                }
            )
    return records


def collect_compilation_evidence(
    build_root: Path,
    project_root: Path,
    started_ns: int,
    required_sources: list[Path] | None = None,
    *,
    artifact_before: dict | None = None,
    previous_records: list[dict] | None = None,
    execution_receipts: list[dict] | None = None,
    build_target: str | None = None,
) -> dict:
    """Require compilation records AND new nonempty objects; filenames in logs are insufficient.

    ``build_target`` is the target requested on the build invocation (``--target X``).
    Records whose object path carries no direct ``CMakeFiles/<target>.dir`` segment
    (e.g. vendor-framework kernel objects written under generated recipe dirs) get the
    invocation-level target attributed with ``target_source="build_invocation"`` — they
    were compiled as part of building that target, which is what coverage matching asks.
    """
    project_root = project_root.resolve()
    records, seen, arches, targets = [], set(), set(), set()
    recorded = set()
    databases = []
    for directory, dirs, files in os.walk(build_root):
        dirs[:] = [
            name
            for name in dirs
            if name not in {".git", ".venv", "node_modules", "__pycache__", "logs"}
        ]
        if "compile_commands.json" in files:
            databases.append(Path(directory) / "compile_commands.json")
    prior = {r.get("object"): r for r in (previous_records or [])}
    inputs = [(db, _read_json(db, [])) for db in databases]
    inputs.append(
        (build_root / "compile_commands.json", make_compilation_records(build_root))
    )
    observed_records = []
    for receipt in execution_receipts or []:
        args = original_compiler_arguments(receipt.get("argv") or [])
        directory = receipt.get("directory")
        if not directory or not _object_compilation(args):
            continue
        sources = [
            arg
            for arg in args
            if Path(arg).suffix.lower() in {".c", ".cc", ".cpp", ".cxx", ".asc", ".cce"}
            and not arg.startswith("-")
        ]
        if len(sources) == 1:
            observed_records.append(
                {
                    "directory": directory,
                    "file": sources[0],
                    "arguments": args,
                    "record_source": "compiler_execution_receipt",
                }
            )
    inputs.append((build_root / "compile_commands.json", observed_records))
    inputs.append(
        (
            build_root / "compile_commands.json",
            [
                {
                    "directory": r["directory"],
                    "file": r["source"],
                    "arguments": r["argv"],
                    "record_source": "previous_owned_execution",
                }
                for r in previous_records or []
                if all(k in r for k in ("directory", "source", "argv"))
            ],
        )
    )
    for db, data in inputs:
        if not isinstance(data, list):
            continue
        for entry in data:
            if not isinstance(entry, dict):
                continue
            args = command_arguments(entry)
            if not args or not _object_compilation(args):
                continue
            args = original_compiler_arguments(args)
            directory = Path(entry.get("directory") or db.parent).resolve()
            source = Path(entry.get("file") or "")
            source = (source if source.is_absolute() else directory / source).resolve()
            obj = _object_path(args, directory, entry)
            try:
                if obj is None or not source.is_file() or not obj.is_file():
                    continue
                stat = obj.stat()
                if stat.st_size == 0:
                    continue
                # Build artefacts from outside this execution root are not evidence.
                obj.resolve().relative_to(build_root.resolve())
            except (OSError, ValueError):
                continue
            deps, dependency_complete = _dependencies(obj, source, directory, args)
            dep_hashes = {}
            try:
                dep_hashes = {
                    str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in deps
                }
                object_hash = hashlib.sha256(obj.read_bytes()).hexdigest()
            except OSError:
                continue
            object_key = str(obj.resolve())
            key = (object_key, tuple(args))
            if key in recorded:
                continue
            stamp = [stat.st_mtime_ns, stat.st_size, object_hash]
            fresh = (
                artifact_before.get(object_key) != stamp
                if artifact_before is not None
                else stat.st_mtime_ns >= started_ns
            )
            executed = receipt_matches(
                args, directory, execution_receipts or [], object_sha256=object_hash
            )
            previous = prior.get(object_key, {})
            cached = (
                dependency_complete
                and previous.get("dependency_complete") is True
                and previous.get("compiler_execution_observed") is True
                and previous.get("argv") == args
                and previous.get("dependency_sha256") == dep_hashes
                and previous.get("object_sha256") == object_hash
            )
            if not (fresh and executed) and not cached:
                continue
            recorded.add(key)
            owned = {p for p in deps if p == project_root or project_root in p.parents}
            if not owned:
                continue
            seen.update(owned)
            arch = command_architectures(args)
            arches.update(arch)
            match = re.search(r"CMakeFiles[/\\]([^/\\]+)\.dir", str(obj))
            if match:
                target = match.group(1)
                target_source = "object_path"
            elif build_target:
                target = build_target
                target_source = "build_invocation"
            else:
                target = None
                target_source = None
            if target:
                targets.add(target)
            records.append(
                {
                    "source": str(source),
                    "object": str(obj.resolve()),
                    "argv": args,
                    "directory": str(directory),
                    "architectures": arch,
                    "target": target,
                    "target_source": target_source,
                    "dependencies": sorted(str(p) for p in owned),
                    "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                    "dependency_sha256": dep_hashes,
                    "object_sha256": object_hash,
                    "dependency_complete": dependency_complete,
                    "compiler_execution_observed": executed
                    or previous.get("compiler_execution_observed", False),
                    "observation": "fresh_object"
                    if fresh
                    else "validated_previous_object",
                    "object_mtime_ns": stat.st_mtime_ns,
                }
            )
    required = {p.resolve() for p in (required_sources or [])}
    missing = sorted(str(p) for p in required - seen)
    return {
        "status": "confirmed" if records and not missing else "unconfirmed",
        "project_root": str(project_root),
        "records": records,
        "required_sources": sorted(str(p) for p in required),
        "receipt_count": len(execution_receipts or []),
        "missing_sources": missing,
        "architectures": sorted(arches),
        "targets": sorted(targets),
        "started_ns": started_ns,
        "reason": None
        if records and not missing
        else "fresh_compilation_of_required_sources_not_observed",
    }


def object_state(root: Path) -> dict:
    """Fingerprint existing objects before invoking the recipe, including future-dated ones."""
    result = {}
    for directory, dirs, files in os.walk(root):
        dirs[:] = [
            d for d in dirs if d not in {".git", ".venv", "node_modules", "logs"}
        ]
        for name in files:
            if not name.endswith((".o", ".obj")):
                continue
            path = Path(directory) / name
            try:
                stat = path.stat()
                result[str(path.resolve())] = [
                    stat.st_mtime_ns,
                    stat.st_size,
                    hashlib.sha256(path.read_bytes()).hexdigest(),
                ]
            except OSError:
                continue
    return result
