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
"""Regenerate an operator skeleton in scratch space and compare it with a project.

用途：在临时目录用 msopgen 重新生成算子骨架，与工程实现做对照取证。
使用方法：由引擎按需调用（compiler_probe_runner），一般不直接手工执行。
"""

from __future__ import annotations

if __package__ in (None, ""):
    import sys as _sys
    from pathlib import Path as _Path

    _sys.path.insert(0, str(_Path(__file__).resolve().parents[2]))


import argparse
import difflib
import os
import shlex
import shutil
import stat
import sys
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

# Direct script execution needs the package root before engine imports.
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from harness.engine.common import run_command  # noqa: E402
from harness.engine.common import which  # noqa: E402
from harness.engine.common import write_json  # noqa: E402
from harness.engine.env_probe import host_arch_summary  # noqa: E402

SKIP_PARTS = {"build", "logs", "__pycache__", ".pytest_cache"}
TEXT_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".json", ".cmake", ".txt"}


@dataclass(frozen=True)
class CommandSpec:
    label: str
    argv: list[str]
    timeout: float = 180.0


def compute_resource(soc: str) -> str:
    value = soc.strip()
    if value.startswith("ai_core-"):
        return value
    if value.startswith("Ascend"):
        value = "ascend" + value[len("Ascend") :]
    return f"ai_core-{value}"


def status_from_returncode(returncode: int) -> str:
    if returncode == 0:
        return "passed"
    if returncode == 124:
        return "timeout"
    if returncode == 126:
        return "permission_error"
    if returncode == 127:
        return "tool_missing"
    return "failed"


def bounded_excerpt(text: str, *, max_lines: int = 80, max_chars: int = 8000) -> str:
    text = text or ""
    lines = text.splitlines()
    selected = "\n".join(lines[:max_lines])
    truncated = len(lines) > max_lines or len(selected) > max_chars
    if len(selected) > max_chars:
        selected = selected[:max_chars]
    if truncated:
        selected = selected.rstrip() + "\n...[truncated]"
    return selected


def command_to_string(argv: list[str]) -> str:
    return " ".join(shlex.quote(part) for part in argv)


def resolve_msopgen(explicit_tool: str | None = None) -> Path | None:
    if explicit_tool:
        return Path(explicit_tool).expanduser().resolve()
    found = which("msopgen") or which("msOpGen")
    return Path(found) if found else None


def tool_facts(path: Path | None) -> dict[str, Any]:
    if path is None:
        return {
            "path": None,
            "exists": False,
            "is_file": False,
            "executable": False,
            "mode_octal": None,
        }

    exists = path.exists()
    mode_octal = None
    if exists:
        mode_octal = oct(stat.S_IMODE(path.stat().st_mode))
    return {
        "path": str(path),
        "exists": exists,
        "is_file": path.is_file() if exists else False,
        "executable": os.access(path, os.X_OK) if exists else False,
        "mode_octal": mode_octal,
    }


def find_input_json(case_dir: Path, explicit_json: Path | None = None) -> Path | None:
    if explicit_json:
        return explicit_json.expanduser()

    search_roots = [
        case_dir / "generated",
        case_dir / "op_dev",
        case_dir,
    ]
    for root in search_roots:
        if not root.exists():
            continue
        matches = sorted(p for p in root.rglob("*.json") if p.is_file())
        if matches:
            return matches[0]
    return None


def build_msopgen_gen_command(
    *,
    tool: str,
    input_json: Path,
    out_dir: Path,
    compute_resource: str,
    language: str = "cpp",
    framework: str | None = None,
    timeout: float = 180.0,
) -> CommandSpec:
    argv = [tool, "gen", "-i", str(input_json)]
    if framework:
        argv.extend(["-f", framework])
    argv.extend(["-c", compute_resource, "-lan", language, "-out", str(out_dir)])
    return CommandSpec("msopgen-gen", argv, timeout=timeout)


def run_command_spec(
    spec: CommandSpec,
    cwd: Path | None = None,
    *,
    max_lines: int = 80,
    max_chars: int = 8000,
) -> dict[str, Any]:
    started = time.time()
    result = run_command(spec.argv, cwd=cwd, timeout=spec.timeout)
    elapsed = round(time.time() - started, 3)
    return {
        "label": spec.label,
        "command": command_to_string(spec.argv),
        "status": status_from_returncode(int(result["returncode"])),
        "returncode": result["returncode"],
        "elapsed_seconds": elapsed,
        "excerpt": bounded_excerpt(
            result.get("output") or "", max_lines=max_lines, max_chars=max_chars
        ),
        "reason": result.get("error"),
    }


def _is_text_candidate(path: Path) -> bool:
    if not path.is_file():
        return False
    if any(part in SKIP_PARTS for part in path.parts):
        return False
    return path.suffix in TEXT_SUFFIXES or path.name == "CMakeLists.txt"


def _candidate_keys(path: Path, root: Path) -> list[str]:
    rel = path.resolve().relative_to(root.resolve()).as_posix()
    keys = [rel]
    for prefix in ("generated/", "op_dev/", "custom_op/"):
        if rel.startswith(prefix):
            keys.append(rel[len(prefix) :])
    parts = rel.split("/")
    for marker in ("op_host", "op_kernel", "framework"):
        if marker in parts:
            keys.append("/".join(parts[parts.index(marker) :]))
    return list(dict.fromkeys(keys))


def _files_by_key(root: Path) -> dict[str, Path]:
    files: dict[str, Path] = {}
    if not root.exists():
        return files
    for path in sorted(p for p in root.rglob("*") if _is_text_candidate(p)):
        for key in _candidate_keys(path, root):
            files.setdefault(key, path)
    return files


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def compare_generated_files(
    case_dir: Path, out_dir: Path, *, max_diffs: int = 8
) -> dict[str, Any]:
    source_files = _files_by_key(case_dir)
    generated_files = _files_by_key(out_dir)
    matched = sorted(set(source_files) & set(generated_files))

    diffs: list[dict[str, Any]] = []
    unchanged = 0
    changed = 0
    for key in matched:
        existing = _read_text(source_files[key])
        regenerated = _read_text(generated_files[key])
        if existing == regenerated:
            unchanged += 1
            continue
        changed += 1
        if len(diffs) >= max_diffs:
            continue
        diff = "\n".join(
            difflib.unified_diff(
                existing.splitlines(),
                regenerated.splitlines(),
                fromfile=f"existing/{key}",
                tofile=f"regenerated/{key}",
                lineterm="",
            )
        )
        diffs.append(
            {
                "relative_path": key,
                "existing_path": str(source_files[key]),
                "regenerated_path": str(generated_files[key]),
                "diff_excerpt": bounded_excerpt(diff, max_lines=80, max_chars=8000),
            }
        )

    return {
        "matched_count": len(matched),
        "changed_count": changed,
        "unchanged_count": unchanged,
        "matched_paths": matched,
        "diffs": diffs,
        "source_file_count": len(source_files),
        "regenerated_file_count": len(generated_files),
    }


def _safe_prepare_output_dir(out_dir: Path) -> None:
    """Reserve a new scratch directory without deleting existing contents."""
    resolved = out_dir.resolve()
    scratch = Path(
        os.environ.get("ACD_SCRATCH_ROOT") or tempfile.gettempdir()
    ).resolve()
    temp = Path(tempfile.gettempdir()).resolve()
    if resolved in {scratch, temp} or not any(
        root in resolved.parents for root in (scratch, temp)
    ):
        raise ValueError(
            f"output must be a new directory strictly below scratch/temp: {out_dir}"
        )
    if resolved.exists() or out_dir.is_symlink():
        raise ValueError(f"refusing to reuse existing output directory: {out_dir}")
    resolved.mkdir(parents=True, exist_ok=False)


def _validate_output_path(path: Path, case_dir: Path) -> None:
    """Keep probe output outside the source project and the installed engine."""
    resolved = path.resolve()
    protected = [case_dir]
    if ROOT.name == "engine":
        package = ROOT.parent.parent if ROOT.parent.name == "scripts" else ROOT.parent
        protected.append(package)
    if any(
        resolved == root or root in resolved.parents or resolved in root.parents
        for root in protected
    ):
        raise ValueError(f"output overlaps a protected project or package: {path}")


def _validate_scratch_root(scratch: Path) -> None:
    """Require scratch storage to be outside the source checkout and package."""
    resolved = scratch.resolve()
    protected = [ROOT]
    if ROOT.name == "engine":
        protected.append(ROOT.parent.parent)
    if any(
        resolved == root or root in resolved.parents or resolved in root.parents
        for root in protected
    ):
        raise ValueError(
            f"scratch root must be outside the project and package: {scratch}"
        )


def interpret_results(data: dict[str, Any]) -> dict[str, Any]:
    tool = data.get("tool") or {}
    input_json = data.get("input_json")
    commands = data.get("commands") or []
    comparison = data.get("comparison") or {}
    gen_command = next(
        (cmd for cmd in commands if cmd.get("label") == "msopgen-gen"), None
    )

    if not tool.get("path") or not tool.get("exists"):
        return {
            "status": "blocked_tool_missing",
            "reason": "msopgen was not found on PATH or at the explicit --tool path.",
            "recommendation": "Keep msopgen deferred and record a tool_missing skip reason.",
            "evidence": ["tool path is missing"],
        }
    if not input_json:
        return {
            "status": "blocked_input_missing",
            "reason": "No operator prototype JSON was available for msopgen gen.",
            "recommendation": "Provide a schema JSON before running regenerate-diff evidence.",
            "evidence": ["input json is missing"],
        }
    if not gen_command or gen_command.get("status") != "passed":
        return {
            "status": "blocked_command_failure",
            "reason": "msopgen gen did not complete successfully.",
            "recommendation": "Keep msopgen out of default diagnosis and report the command status as probe evidence.",
            "evidence": [
                f"{cmd.get('label')}={cmd.get('status')} rc={cmd.get('returncode')}"
                for cmd in commands
            ],
        }

    matched = int(comparison.get("matched_count") or 0)
    changed = int(comparison.get("changed_count") or 0)
    unchanged = int(comparison.get("unchanged_count") or 0)
    if changed:
        return {
            "status": "regenerated_with_diffs",
            "reason": f"msopgen regenerated the project and produced diff evidence for {changed} comparable file(s).",
            "recommendation": "Use this only as isolated regenerate-diff evidence; do not mutate user projects in default diagnosis.",
            "evidence": [
                f"matched_count={matched}",
                f"changed_count={changed}",
                f"unchanged_count={unchanged}",
            ],
        }
    if matched:
        return {
            "status": "regenerated_clean",
            "reason": "msopgen regenerated comparable files and they match the existing files.",
            "recommendation": "Record clean regenerate evidence for schema/generated-code cases when explicitly triggered.",
            "evidence": [f"matched_count={matched}", "changed_count=0"],
        }
    return {
        "status": "regenerated_no_comparable_files",
        "reason": "msopgen generated a project, but no comparable generated files exist in the source project.",
        "recommendation": "Record tool availability without claiming evidence of a source difference.",
        "evidence": [
            f"regenerated_file_count={comparison.get('regenerated_file_count', 0)}"
        ],
    }


def run_poc(
    *,
    case_dir: Path,
    json_out: Path | None = None,
    md_out: Path | None = None,
    input_json: Path | None = None,
    explicit_tool: str | None = None,
    out_dir: Path | None = None,
    soc: str = "Ascend910B4",
    language: str = "cpp",
    framework: str | None = None,
    command_timeout_s: float = 180.0,
    max_lines: int = 80,
    max_chars: int = 8000,
) -> dict[str, Any]:
    started = datetime.now(tz=timezone.utc)
    case_dir = case_dir.resolve()
    if not case_dir.is_dir():
        raise ValueError(f"project directory does not exist: {case_dir}")
    for report_path in (json_out, md_out):
        if report_path is not None:
            _validate_output_path(report_path, case_dir)
    selected_json = find_input_json(
        case_dir, input_json.resolve() if input_json else None
    )
    tool_path = resolve_msopgen(explicit_tool)
    facts = tool_facts(tool_path)
    scratch = Path(
        os.environ.get("ACD_SCRATCH_ROOT") or tempfile.gettempdir()
    ).resolve()
    _validate_scratch_root(scratch)
    if out_dir is not None:
        _validate_output_path(out_dir, case_dir)
        _safe_prepare_output_dir(out_dir)
    scratch.mkdir(parents=True, exist_ok=True)
    work_dir = Path(tempfile.mkdtemp(prefix="msopgen-", dir=scratch))
    output_dir = out_dir.resolve() if out_dir is not None else work_dir / "regenerated"
    if out_dir is None:
        output_dir.mkdir()
    commands: list[dict[str, Any]] = []
    comparison: dict[str, Any] = {
        "matched_count": 0,
        "changed_count": 0,
        "unchanged_count": 0,
        "matched_paths": [],
        "diffs": [],
        "source_file_count": 0,
        "regenerated_file_count": 0,
    }

    if selected_json and selected_json.is_file() and tool_path and facts.get("is_file"):
        # Some SDK tools write incidental logs or modify their input. Give the
        # generator a private input copy and working directory in addition to -out.
        probe_json = work_dir / selected_json.name
        shutil.copy2(selected_json, probe_json)
        spec = build_msopgen_gen_command(
            tool=str(tool_path),
            input_json=probe_json,
            out_dir=output_dir,
            compute_resource=compute_resource(soc),
            language=language,
            framework=framework,
            timeout=command_timeout_s,
        )
        commands.append(
            run_command_spec(
                spec, cwd=work_dir, max_lines=max_lines, max_chars=max_chars
            )
        )
        if commands[-1]["status"] == "passed":
            comparison = compare_generated_files(case_dir, output_dir)

    data: dict[str, Any] = {
        "started_at": started.isoformat(),
        "finished_at": datetime.now(tz=timezone.utc).isoformat(),
        "case": str(case_dir),
        "input_json": str(selected_json) if selected_json else None,
        "output_dir": str(output_dir),
        "work_dir": str(work_dir),
        "tool": facts,
        "environment": {"host": host_arch_summary()},
        "soc": soc,
        "compute_resource": compute_resource(soc),
        "language": language,
        "framework": framework,
        "commands": commands,
        "comparison": comparison,
    }
    data["conclusion"] = interpret_results(data)

    if json_out:
        write_json(json_out, data)
    if md_out:
        md_out.parent.mkdir(parents=True, exist_ok=True)
        md_out.write_text(render_markdown(data), encoding="utf-8")
    return data


def render_markdown(data: dict[str, Any]) -> str:
    conclusion = data.get("conclusion") or {}
    comparison = data.get("comparison") or {}
    tool = data.get("tool") or {}
    host = (data.get("environment") or {}).get("host") or {}

    lines = ["# msopgen regeneration comparison\n\n"]
    lines.append(f"- Host: `{host.get('system')} / {host.get('machine')}`\n")
    lines.append(f"- Case: `{data.get('case')}`\n")
    lines.append(f"- Input JSON: `{data.get('input_json')}`\n")
    lines.append(f"- Output dir: `{data.get('output_dir')}`\n")
    lines.append(f"- Tool: `{tool.get('path')}`\n")
    lines.append(f"- Tool exists: `{tool.get('exists')}`\n")
    lines.append(f"- Tool executable: `{tool.get('executable')}`\n")
    lines.append(f"- SOC: `{data.get('soc')}`\n")
    lines.append(f"- Compute resource: `{data.get('compute_resource')}`\n")
    lines.append(f"- Conclusion: `{conclusion.get('status')}`\n\n")

    lines.append("## Conclusion\n\n")
    lines.append(f"- Reason: {conclusion.get('reason')}\n")
    lines.append(f"- Recommendation: {conclusion.get('recommendation')}\n")
    evidence = conclusion.get("evidence") or []
    if evidence:
        lines.append("- Evidence:\n")
        for item in evidence:
            lines.append(f"  - {item}\n")
    lines.append("\n")

    lines.append("## Comparison\n\n")
    lines.append(f"- Source file count: `{comparison.get('source_file_count')}`\n")
    lines.append(
        f"- Regenerated file count: `{comparison.get('regenerated_file_count')}`\n"
    )
    lines.append(f"- Matched count: `{comparison.get('matched_count')}`\n")
    lines.append(f"- Changed count: `{comparison.get('changed_count')}`\n")
    lines.append(f"- Unchanged count: `{comparison.get('unchanged_count')}`\n\n")

    lines.append("## Commands\n\n")
    commands = data.get("commands") or []
    if not commands:
        lines.append(
            "No commands were run because the tool or input JSON was unavailable.\n\n"
        )
    for cmd in commands:
        lines.append(f"### `{cmd.get('label')}`\n\n")
        lines.append(f"- Status: `{cmd.get('status')}`\n")
        lines.append(f"- Return code: `{cmd.get('returncode')}`\n")
        lines.append(f"- Command: `{cmd.get('command')}`\n")
        if cmd.get("reason"):
            lines.append(f"- Reason: `{cmd.get('reason')}`\n")
        excerpt = cmd.get("excerpt") or ""
        if excerpt:
            lines.append("\n```text\n")
            lines.append(excerpt)
            lines.append("\n```\n")
        lines.append("\n")

    diffs = comparison.get("diffs") or []
    if diffs:
        lines.append("## Diff Excerpts\n\n")
        for item in diffs:
            lines.append(f"### `{item.get('relative_path')}`\n\n")
            lines.append(f"- Existing: `{item.get('existing_path')}`\n")
            lines.append(f"- Regenerated: `{item.get('regenerated_path')}`\n\n")
            lines.append("```diff\n")
            lines.append(item.get("diff_excerpt") or "")
            lines.append("\n```\n\n")
    return "".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Regenerate an operator skeleton in scratch space and compare files."
    )
    parser.add_argument(
        "--case", required=True, help="Source project directory (read-only)."
    )
    parser.add_argument("--json-out", required=True)
    parser.add_argument("--md-out", required=True)
    parser.add_argument("--input-json", help="Explicit operator prototype JSON.")
    parser.add_argument("--tool", help="Explicit msopgen path.")
    parser.add_argument(
        "--out-dir",
        help="New output directory strictly below ACD_SCRATCH_ROOT or system temp; existing paths are rejected.",
    )
    parser.add_argument("--soc", default="Ascend910B4")
    parser.add_argument("--language", default="cpp")
    parser.add_argument("--framework")
    parser.add_argument(
        "--timeout", type=float, default=180.0, help="msopgen command timeout seconds"
    )
    parser.add_argument("--max-lines", type=int, default=80)
    parser.add_argument("--max-chars", type=int, default=8000)
    args = parser.parse_args()

    data = run_poc(
        case_dir=Path(args.case),
        json_out=Path(args.json_out),
        md_out=Path(args.md_out),
        input_json=Path(args.input_json) if args.input_json else None,
        explicit_tool=args.tool,
        out_dir=Path(args.out_dir) if args.out_dir else None,
        soc=args.soc,
        language=args.language,
        framework=args.framework,
        command_timeout_s=args.timeout,
        max_lines=args.max_lines,
        max_chars=args.max_chars,
    )
    print(str(Path(args.json_out)))
    print(str(Path(args.md_out)))
    print(data["conclusion"]["status"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
