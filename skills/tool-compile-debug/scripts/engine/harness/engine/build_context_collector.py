# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
from pathlib import Path
from typing import Any

from harness.engine.common import error_payload
from harness.engine.common import read_text
from harness.engine.common import resolve_project_source
from harness.engine.common import write_json
from harness.engine.env_probe import detect_cann
from harness.engine.source_facts import SourceReader
from harness.engine.response_files import expand_response_arguments

SOURCE_PATTERNS = [
    "*.asc",
    "*.cpp",
    "*.cc",
    "*.cxx",
    "*.c",
    "*.cce",
    "*.h",
    "*.hpp",
    "*.hh",
    "*.json",
    "*.ini",
]

# Capture a literal project target for comparison with the requested SoC and
# build-log evidence. This does not evaluate CMake variables or conditionals.
_SOC_CMAKE_RE = re.compile(
    r"set\s*\(\s*SOC_VERSION\s+\"?([A-Za-z0-9_\-]+)\"?", re.IGNORECASE
)


def read_project_soc_version(case_dir: Path) -> str | None:
    """读工程 CMakeLists.txt 里声明的 ``SOC_VERSION``；未声明/不可读返回 None。"""
    try:
        path = resolve_project_source("CMakeLists.txt", case_dir)
        text = read_text(path) if path else ""
    except OSError:
        return None
    m = _SOC_CMAKE_RE.search(text or "")
    return m.group(1) if m else None


# JSON operator prototypes are source inputs; evaluation annotations and previous
# diagnosis outputs are not. Exclude the latter before collecting source text.
_CASE_ANSWER_FILES = frozenset(
    {
        "diagnosis.gold.json",
        "meta.json",
        "case_source.json",
        "localization.gold.json",
    }
)


def _is_case_metadata(rel: str) -> bool:
    """Identify evaluation annotations and prior outputs outside the source inputs."""
    norm = rel.replace("\\", "/")
    if Path(norm).name in _CASE_ANSWER_FILES:
        return True
    # Previous logs and diagnostic outputs must not become current source evidence.
    return norm.startswith("logs/") or "/logs/" in norm


def _find_files(
    case_dir: Path, pattern: str, excluded: list[str] | None = None
) -> list[str]:
    found = []
    for path in case_dir.rglob(pattern):
        if not path.is_file():
            continue
        relative = path.relative_to(case_dir).as_posix()
        if _is_case_metadata(relative):
            continue
        resolved = resolve_project_source(path, case_dir)
        if resolved is None:
            if excluded is not None:
                excluded.append(relative)
            continue
        found.append(resolved.relative_to(case_dir).as_posix())
    return sorted(set(found))


def _collect_source_files(case_dir: Path) -> dict[str, list[str]]:
    all_sources: list[str] = []
    excluded: list[str] = []
    for pattern in SOURCE_PATTERNS:
        all_sources.extend(_find_files(case_dir, pattern, excluded))
    all_sources = sorted(
        {p for p in all_sources if not ({"build", "logs", ".git"} & set(Path(p).parts))}
    )
    op_host = [
        p
        for p in all_sources
        if "op_host" in p.lower() or p.lower().endswith("_host.cpp")
    ]
    op_kernel = [
        p
        for p in all_sources
        if "op_kernel" in p.lower()
        or p.lower().endswith(".asc")
        or "kernel" in p.lower()
    ]
    generated = [
        p
        for p in all_sources
        if any(x in p.lower() for x in ["generated", "autogen", "build_out", "output"])
    ]
    prototypes = [
        p
        for p in all_sources
        if p.lower().endswith((".json", ".ini")) and "logs/" not in p
    ]
    return {
        "all_sources": all_sources,
        "op_host_files": op_host,
        "op_kernel_files": op_kernel,
        "generated_files": generated,
        "prototype_files": prototypes,
        "excluded_external_sources": sorted(set(excluded)),
    }


def _read_make_variables(
    path: Path, reader: SourceReader | None = None
) -> dict[str, str]:
    variables: dict[str, str] = {}
    text = _optional_text(reader, path) if reader else read_text(path)
    for line in text.splitlines():
        match = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$", line)
        if match:
            variables[match.group(1)] = match.group(2).strip()
    return variables


def _expand_make_variables(command: str, variables: dict[str, str]) -> str:
    def repl(match: re.Match[str]) -> str:
        return variables.get(match.group(1), match.group(0))

    return re.sub(r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)", repl, command)


def _source_from_command(command: str) -> str | None:
    try:
        parts = shlex.split(command, posix=(os.name != "nt"))
    except Exception:
        parts = command.split()
    for part in reversed(parts):
        if part.endswith((".asc", ".cpp", ".cc", ".cxx")):
            return part
    return None


def _collect_compile_commands_from_build_make(
    case_dir: Path,
    build_dir: Path | None = None,
    *,
    reader: SourceReader | None = None,
) -> list[dict[str, str]]:
    build_dir = build_dir or case_dir / "build"
    if not build_dir.exists():
        return []
    commands: list[dict[str, str]] = []
    for build_make in sorted(build_dir.rglob("build.make")):
        if (
            reader.resolve(build_make)
            if reader
            else resolve_project_source(build_make, case_dir)
        ) is None:
            continue
        flags = build_make.parent / "flags.make"
        if (
            flags.exists()
            and (
                reader.resolve(flags)
                if reader
                else resolve_project_source(flags, case_dir)
            )
            is None
        ):
            continue
        variables = _read_make_variables(flags, reader)
        text = _optional_text(reader, build_make) if reader else read_text(build_make)
        for raw_line in text.splitlines():
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if not ("bisheng" in line or "ccec" in line):
                continue
            if " -c " not in f" {line} ":
                continue
            command = " ".join(_expand_make_variables(line, variables).split())
            source = _source_from_command(command)
            if not source:
                continue
            commands.append(
                {
                    "directory": str(build_dir),
                    "command": command,
                    "file": source,
                }
            )
    return commands


def _optional_text(reader: SourceReader, path: Path) -> str:
    return (reader.read(path) or "") if path.exists() else ""


def _metadata_json(reader: SourceReader, path: Path, gaps: list, kind: str):
    text = reader.read(path)
    if text is None:
        return None
    try:
        value = json.loads(text)
        if value is None:
            gaps.append({"path": str(path), "reason": "invalid_" + kind})
        return value
    except (ValueError, RecursionError):
        gaps.append({"path": str(path), "reason": "invalid_" + kind})
        return None


def _command_record(
    original,
    origin: str,
    selected: Path,
    gaps: list,
    reader: SourceReader,
    response_budget: dict,
) -> dict | None:
    """Normalize database and Make evidence through the same identity contract."""

    def invalid():
        gaps.append({"path": origin, "reason": "invalid_compile_command"})
        return None

    if not isinstance(original, dict):
        return invalid()
    if not isinstance(original.get("file"), str) or not original["file"]:
        return invalid()
    if any(
        original.get(k) is not None and not isinstance(original[k], str)
        for k in ("directory", "command", "output", "target")
    ):
        return invalid()
    row = dict(original)
    directory = Path(row.get("directory") or selected)
    if not directory.is_absolute():
        directory = selected / directory
    row["directory"] = str(directory.resolve())
    row["evidence_source"] = origin
    argv = row.get("arguments")
    if argv is not None and (
        not isinstance(argv, list)
        or not argv
        or not all(isinstance(v, str) for v in argv)
    ):
        return invalid()
    if argv is None:
        try:
            argv = shlex.split(row.get("command") or "")
        except ValueError:
            gaps.append({"path": origin, "reason": "unparsed_command"})
            argv = []
    original_argv = list(argv)
    row["invocation_id"] = hashlib.sha256(
        json.dumps(
            {
                "cwd": row["directory"],
                "argv": original_argv,
                "output": row.get("output"),
            },
            sort_keys=True,
        ).encode()
    ).hexdigest()
    gap_start = len(reader.gaps)
    response = expand_response_arguments(
        argv,
        directory.resolve(),
        reader,
        max_tokens=min(4096, response_budget["remaining"]),
        max_response_bytes=min(4194304, response_budget["bytes_remaining"]),
    )
    response_budget["remaining"] = max(
        0, response_budget["remaining"] - response.get("tokens_visited", 0)
    )
    if response_budget["remaining"] == 0:
        for gap in response["gaps"]:
            if gap["reason"] == "response_token_budget":
                gap["reason"] = "response_query_token_budget"
    response_budget["bytes_remaining"] = max(
        0,
        response_budget["bytes_remaining"] - response.get("response_bytes_charged", 0),
    )
    if response_budget["bytes_remaining"] == 0:
        for gap in response["gaps"]:
            if gap["reason"] == "response_expansion_byte_budget":
                gap["reason"] = "response_query_byte_budget"
    for gap in reader.gaps[gap_start:]:
        gap.update(purpose="response_file", invocation_id=row["invocation_id"])
        response["gaps"].append(dict(gap))
    argv = response["arguments"]
    row["original_arguments"] = original_argv
    row["arguments"] = argv
    row["response_files"] = response["files"]
    row["response_expansion"] = {
        k: v for k, v in response.items() if k not in {"arguments", "files"}
    }
    output = str(row.get("output") or " ".join(argv)).replace("\\", "/")
    target_match = re.search(r"CMakeFiles/([^/]+)\.dir/", output)
    row["target"] = row.get("target") or (
        target_match.group(1) if target_match else None
    )
    row["unresolved"] = [
        v
        for v in argv
        if "$" in v or v.startswith("@") or v in {"&&", "||", ";", "|", ">", ">>", "<"}
    ]
    # Token completeness only; no freshness, executability or failure proof.
    row["replayable"] = (
        bool(argv) and not row["unresolved"] and response["status"] != "partial"
    )
    row["configuration_id"] = (
        hashlib.sha256(
            json.dumps(
                {
                    "invocation_id": row["invocation_id"],
                    "argv": argv,
                    "response_files": response["files"],
                    "response_status": response["status"],
                    "response_gaps": response["gaps"],
                },
                sort_keys=True,
            ).encode()
        ).hexdigest()
        if response["status"] != "not_needed"
        else row["invocation_id"]
    )
    return row


def select_compile_commands(
    facts: dict,
    *,
    source: Path | None = None,
    target: str | None = None,
    configuration_id: str | None = None,
) -> list[dict]:
    selected = []
    for command in facts["compile_commands"]:
        path = Path(command["file"])
        path = path if path.is_absolute() else Path(command["directory"]) / path
        if source is not None and path.resolve() != source.resolve():
            continue
        if target and command.get("target") != target:
            continue
        if configuration_id and command.get("configuration_id") != configuration_id:
            continue
        selected.append(command)
    return selected


def selected_build_directory(case_dir: Path, build_dir: Path | None = None) -> Path:
    """Only an explicit build selection may extend project read scope."""
    case_dir = case_dir.resolve()
    selected = Path(build_dir) if build_dir else case_dir / "build"
    selected = (
        (case_dir / selected).resolve()
        if not selected.is_absolute()
        else selected.resolve()
    )
    if build_dir is None and not selected.is_relative_to(case_dir):
        raise ValueError(
            "default build points outside project; select --build-dir explicitly"
        )
    return selected


def build_metadata_reader(
    case_dir: Path, build_dir: Path | None = None, *, max_files=128, max_bytes=4194304
) -> SourceReader:
    selected = selected_build_directory(case_dir, build_dir)
    return SourceReader(
        case_dir,
        max_files=max_files,
        max_bytes=max_bytes,
        additional_roots=[selected] if build_dir is not None else [],
    )


def build_facts(
    case_dir: Path,
    build_dir: Path | None = None,
    *,
    metadata_reader: SourceReader | None = None,
) -> dict:
    """Read existing build evidence without configuring or executing recipes."""
    case_dir = case_dir.resolve()
    selected = selected_build_directory(case_dir, build_dir)
    reader = metadata_reader or build_metadata_reader(case_dir, build_dir)
    if not reader.within_scope(selected):
        raise ValueError("selected build is outside the supplied metadata reader scope")
    databases = [selected / "compile_commands.json"]
    if build_dir is None:
        databases.append(case_dir / "logs/compile_commands.json")
    rows = []
    sources = []
    gaps = []
    response_budget = {"remaining": 32768, "bytes_remaining": 8388608}
    for database in databases:
        if database.exists():
            data = _metadata_json(reader, database, gaps, "compile_database")
            if data is None:
                continue
            if not isinstance(data, list):
                gaps.append(
                    {"path": str(database), "reason": "invalid_compile_database"}
                )
                continue
            sources.append(str(database))
            for original in data:
                row = _command_record(
                    original, str(database), selected, gaps, reader, response_budget
                )
                if row is not None:
                    rows.append(row)
    if not rows:
        for original in _collect_compile_commands_from_build_make(
            case_dir, selected, reader=reader
        ):
            row = _command_record(
                original, "generated_make", selected, gaps, reader, response_budget
            )
            if row is not None:
                rows.append(row)
    if not rows:
        gaps.append({"path": str(selected), "reason": "no_compile_commands"})
    # Existing CMake File API replies are configuration evidence, not source guesses.
    targets = []
    reply = selected / ".cmake/api/v1/reply"
    for model in sorted(reply.glob("codemodel-v2-*.json")) if reply.is_dir() else []:
        data = _metadata_json(reader, model, gaps, "file_api")
        if data is None:
            continue
        if not isinstance(data, dict) or not isinstance(
            data.get("configurations"), list
        ):
            gaps.append({"path": str(model), "reason": "invalid_file_api"})
            continue
        for config in data["configurations"]:
            if not isinstance(config, dict) or not isinstance(
                config.get("targets", []), list
            ):
                gaps.append({"path": str(model), "reason": "invalid_file_api"})
                continue
            for target in config.get("targets", []):
                if not isinstance(target, dict) or not isinstance(
                    target.get("name"), str
                ):
                    gaps.append({"path": str(model), "reason": "invalid_file_api"})
                    continue
                item = {
                    "name": target.get("name"),
                    "configuration": config.get("name"),
                    "source": str(model),
                    "id": target.get("id"),
                }
                detail = reply / str(target.get("jsonFile", ""))
                if detail.is_file():
                    item["detail"] = _metadata_json(reader, detail, gaps, "file_api")
                targets.append(item)
    return {
        "build_dir": str(selected),
        "compile_commands": rows,
        "databases": sources,
        "configured_targets": targets,
        "gaps": gaps + [g for g in reader.gaps if g.get("purpose") != "response_file"],
        "freshness": "unknown",
        "response_token_budget": {
            "limit": 32768,
            "remaining": response_budget["remaining"],
        },
        "response_byte_budget": {
            "limit": 8388608,
            "remaining": response_budget["bytes_remaining"],
        },
        "metadata_read_evidence": reader.summary(),
    }


def collect_context(
    case_dir: Path,
    *,
    primary_error: dict | None = None,
    cann: dict | None = None,
    build_dir: Path | None = None,
    focused: bool = False,
    max_files: int = 80,
    max_bytes: int = 2097152,
    target: str | None = None,
    configuration_id: str | None = None,
    max_metadata_files: int = 128,
    max_metadata_bytes: int = 4194304,
) -> dict[str, Any]:
    case_dir = Path(case_dir).resolve()
    files = (
        _collect_source_files(case_dir)
        if not focused
        else {
            name: []
            for name in (
                "all_sources",
                "op_host_files",
                "op_kernel_files",
                "generated_files",
                "prototype_files",
                "excluded_external_sources",
            )
        }
    )
    metadata = build_metadata_reader(
        case_dir, build_dir, max_files=max_metadata_files, max_bytes=max_metadata_bytes
    )
    facts = build_facts(case_dir, build_dir, metadata_reader=metadata)
    selected_build = Path(facts["build_dir"])
    cmake_lists = case_dir / "CMakeLists.txt"
    cmake_cache = selected_build / "CMakeCache.txt"
    compile_commands = selected_build / "compile_commands.json"
    if not compile_commands.exists():
        compile_commands = case_dir / "logs" / "compile_commands.json"

    compile_data = facts["compile_commands"]
    build_sh = case_dir / "build.sh"
    cache_text = _optional_text(metadata, cmake_cache)
    generator = "unknown"
    m = re.search(r"CMAKE_GENERATOR:INTERNAL=(.+)", cache_text)
    if m:
        generator = m.group(1).strip()
    elif (selected_build / "build.ninja").exists():
        generator = "Ninja"
    elif (selected_build / "Makefile").exists():
        generator = "Unix Makefiles"

    target_names = []
    cmake_text = _optional_text(metadata, cmake_lists)
    soc_match = _SOC_CMAKE_RE.search(cmake_text)
    for match in re.finditer(
        r"add_(?:executable|library)\s*\(\s*([A-Za-z0-9_\-]+)", cmake_text
    ):
        target_names.append(match.group(1))

    env_keys = [
        "ASCEND_CANN_PACKAGE_PATH",
        "ASCEND_TOOLKIT_HOME",
        "ASCEND_HOME_PATH",
        "CMAKE_PREFIX_PATH",
        "LD_LIBRARY_PATH",
        "PYTHONPATH",
        "SOC_VERSION",
        "CMAKE_ASC_ARCHITECTURES",
    ]
    warnings: list[str] = []
    if files["excluded_external_sources"]:
        warnings.append(
            "Source links outside the explicit project were excluded from project source evidence."
        )
    if not cmake_lists.exists() and not build_sh.exists():
        warnings.append("No CMakeLists.txt or build.sh found.")
    if not compile_commands.exists() and not compile_data:
        warnings.append("compile_commands.json not found.")

    context: dict[str, Any] = {
        "case_dir": str(case_dir),
        "meta": {},  # Evaluation labels are not compile evidence.
        "files": {
            "cmake_lists": str(cmake_lists) if cmake_lists.exists() else None,
            "cmake_presets": str(case_dir / "CMakePresets.json")
            if (case_dir / "CMakePresets.json").exists()
            else None,
            "cmake_cache": str(cmake_cache) if cmake_cache.exists() else None,
            "cmake_output_log": str(selected_build / "CMakeFiles" / "CMakeOutput.log")
            if (selected_build / "CMakeFiles" / "CMakeOutput.log").exists()
            else None,
            "cmake_error_log": str(selected_build / "CMakeFiles" / "CMakeError.log")
            if (selected_build / "CMakeFiles" / "CMakeError.log").exists()
            else None,
            "compile_commands": str(compile_commands)
            if compile_commands.exists()
            else None,
            "build_sh": str(build_sh) if build_sh.exists() else None,
            **files,
        },
        "file_text": {},
        "env": {k: os.environ.get(k) for k in env_keys if os.environ.get(k)},
        "cann": cann if cann is not None else detect_cann(),
        "primary_error": primary_error or {},
        "soc_version": soc_match.group(1) if soc_match else None,
        "build": {
            "generator": generator,
            "targets": sorted(set(target_names)),
            "compile_commands_count": len(compile_data)
            if isinstance(compile_data, list)
            else 0,
            "compile_commands_excerpt": compile_data[:20]
            if isinstance(compile_data, list)
            else [],
            "compile_commands": compile_data if isinstance(compile_data, list) else [],
        },
        "warnings": warnings,
        "build_facts": facts,
    }
    reader = SourceReader(
        case_dir,
        max_files=max_files,
        max_bytes=max_bytes,
        additional_roots=[selected_build] if build_dir is not None else [],
    )
    context["file_text"] = reader.texts
    # The failing file must survive the preview budget even in a large tree.
    failed = primary_error or {}
    cann_root = (context["cann"] or {}).get("install_dir")
    failed_path = (
        reader.resolve(failed.get("file") or "")
        or resolve_project_source(
            failed.get("file") or "",
            case_dir,
            reanchor=True,
            external_roots=[cann_root] if cann_root else [],
        )
        if failed.get("file_resolved") is not False
        else None
    )
    selected = [reader.key(failed_path)] if failed_path else []
    if failed.get("file") and failed_path is None:
        reader.gaps.append(
            {"path": failed["file"], "reason": "failed_source_not_resolved"}
        )
        context["primary_error"] = {**failed, "file_resolved": False}
    selected_commands = (
        select_compile_commands(
            facts,
            source=failed_path,
            target=target,
            configuration_id=configuration_id,
        )
        if failed_path or not failed.get("file")
        else []
    )
    context["selected_commands"] = selected_commands
    context["configuration_status"] = (
        "selected"
        if len(selected_commands) == 1
        else "ambiguous"
        if selected_commands
        else "unbound"
    )
    context["configuration_request"] = {
        "target": target,
        "configuration_id": configuration_id,
    }
    include_dirs = []
    for command in selected_commands:
        argv = command.get("arguments") or []
        directory = Path(command.get("directory") or selected_build)
        for i, arg in enumerate(argv):
            value = (
                argv[i + 1]
                if arg in ("-I", "-iquote", "-isystem") and i + 1 < len(argv)
                else arg[2:]
                if arg.startswith("-I")
                else None
            )
            if value:
                p = Path(value)
                include_dirs.append(p if p.is_absolute() else directory / p)
    context["include_candidates"] = []
    for rel in selected:
        context["include_candidates"].extend(
            reader.include_closure(case_dir / rel, include_dirs=include_dirs)
        )
    # Only after the failure and direct include closure may scripts use source slots.
    for rel in ["CMakeLists.txt", "build.sh"]:
        path = case_dir / rel
        if resolve_project_source(path, case_dir):
            reader.read(path)
    for rel in (
        files["op_host_files"][:20]
        + files["op_kernel_files"][:20]
        + files["prototype_files"][:20]
    ):
        path = case_dir / rel
        if resolve_project_source(path, case_dir):
            reader.read(path)

    # Shared declarations need not live under host/kernel directories. Include
    # additional C/C++ sources for cross-file localization, within a fixed file
    # budget. Report omitted files below so incomplete coverage stays explicit.
    _CPP_SUFFIXES = (".asc", ".cpp", ".cc", ".cxx", ".h", ".hpp")
    MAX_EXTRA_SOURCE_FILES = 60
    extra_loaded = 0
    for rel in files["all_sources"]:
        if extra_loaded >= MAX_EXTRA_SOURCE_FILES:
            break
        if rel in context["file_text"]:
            continue
        if not rel.lower().endswith(_CPP_SUFFIXES):
            continue
        path = case_dir / rel
        if resolve_project_source(path, case_dir):
            reader.read(path)
            extra_loaded += 1
    if focused:
        files["all_sources"] = [
            p
            for p in reader.texts
            if Path(p).suffix
            in {".asc", ".cpp", ".c", ".cce", ".cc", ".cxx", ".h", ".hpp", ".hh"}
        ]
    context["read_evidence"] = reader.summary()
    context["metadata_read_evidence"] = metadata.summary()
    context["interpretation_id"] = hashlib.sha256(
        json.dumps(
            {
                "files": reader.facts,
                "commands": selected_commands,
                "cann": context["cann"],
                "requested": context["configuration_request"],
            },
            sort_keys=True,
        ).encode()
    ).hexdigest()
    context["build"]["targets"] = sorted(
        set(
            target_names
            + [t["name"] for t in facts["configured_targets"] if t.get("name")]
        )
    )
    context["source_coverage"] = {
        "available": len(files["all_sources"]),
        "loaded": sum(rel in context["file_text"] for rel in files["all_sources"]),
        "omitted_sources": [
            rel for rel in files["all_sources"] if rel not in context["file_text"]
        ],
        "failed_source_loaded": any(rel in context["file_text"] for rel in selected),
        "failed_source_reanchored": bool(failed.get("file_reanchored"))
        or bool(failed_path and reader.resolve(failed.get("file") or "") is None),
        "excluded_external_sources": files["excluded_external_sources"],
    }
    return context


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Collect build context for an Ascend C case."
    )
    parser.add_argument("--case", required=True)
    parser.add_argument("--json-out", required=True)
    args = parser.parse_args()
    try:
        data = collect_context(Path(args.case).resolve())
    except Exception as exc:
        data = {
            "case_dir": args.case,
            "files": {},
            "env": {},
            "build": {},
            "warnings": [],
            **error_payload("build_context_collector", exc, case=args.case),
        }
    write_json(Path(args.json_out), data)
    print(str(Path(args.json_out)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
