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
import shutil
import subprocess
import tempfile
import time
import uuid
from pathlib import Path
from typing import Any

from harness.engine.build_context_collector import read_project_soc_version
from harness.engine.build_receipts import compiler_receipts
from harness.engine.build_receipts import instrument_cmake
from harness.engine.build_receipts import read_receipts
from harness.engine.build_receipts import trace_command
from harness.engine.common import PROJECT_ROOT
from harness.engine.common import KnowledgeBaseIntegrityError
from harness.engine.common import error_payload
from harness.engine.common import force_utf8_stdio
from harness.engine.common import load_api_signature_kb
from harness.engine.common import load_json
from harness.engine.common import read_text
from harness.engine.common import write_json
from harness.engine.env_probe import detect_cann
from harness.engine.execution import execute_recipe, observe_run, _alive
from harness.engine import verification_recovery as recovery
from harness.engine.log_normalizer import normalize_log_text
from harness.engine.verification_evidence import collect_compilation_evidence
from harness.engine.verification_evidence import compare_issues
from harness.engine.verification_evidence import digest
from harness.engine.verification_evidence import failure_evidence
from harness.engine.verification_evidence import identifier_retention
from harness.engine.verification_evidence import object_state
from harness.engine.verification_evidence import prior_failure_coverage

# Version guidance is derived from the packaged domain registry.
from harness.engine.version_gate import classify as classify_version
from harness.engine.version_gate import is_mismatch
from harness.engine.version_gate import normalize_version


class PatchValidationError(Exception):
    """Raised when patch content or a project directory fails validation."""


ENV_OVERRIDE_HEADER = "---env---"
_ENV_KEY_RE = re.compile(r"^[A-Z_][A-Z0-9_]*$")


def _parse_env_override(text: str) -> dict[str, str] | None:
    """Parse an optional environment override header into a validated key-value mapping. Invalid keys raise PatchValidationError."""
    if not text:
        return None
    first_line = text.split("\n", 1)[0].strip()
    if first_line != ENV_OVERRIDE_HEADER:
        return None
    env: dict[str, str] = {}
    for raw in text.splitlines()[1:]:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise PatchValidationError(f"env override line missing '=': {line!r}")
        key, _, value = line.partition("=")
        key = key.strip()
        value = value.strip()
        if not _ENV_KEY_RE.match(key):
            raise PatchValidationError(f"env override has invalid KEY: {key!r}")
        env[key] = os.path.expandvars(value)
    if not env:
        raise PatchValidationError(
            "env override header present but no KEY=VALUE entries"
        )
    return env


_PATCH_ALLOWED_LINE_FIRST_CHARS = frozenset({"-", "+", "@", " ", "\\"})


def _validate_patch_text(text: str) -> None:
    """拒绝不像 unified diff 的 patch 文本（防 RCE / 注入）。

    规则（与 GNU patch 的隐式解析对齐）：
      * 文本必须含至少一个 ``--- `` 头部行；
      * 从第一个 ``--- `` 开始到结尾，每一非空行的首字符必须属于
        ``{'-', '+', '@', ' ', '\\\\'}``——即标准 unified diff 行；
      * 头部之前的 preamble 不审（GNU patch 会忽略），但本函数仍会要求
        头部存在，避免空 patch 或纯命令 payload 静默通过。
    """
    if "--- " not in text:
        raise PatchValidationError("patch text does not contain a '--- ' header")
    head_idx = text.index("--- ")
    body = text[head_idx:]
    for lineno, line in enumerate(body.splitlines(), start=1):
        if not line:
            continue
        if line[0] not in _PATCH_ALLOWED_LINE_FIRST_CHARS:
            raise PatchValidationError(
                f"patch line {lineno} starts with disallowed character "
                f"{line[0]!r}: {line[:80]!r}"
            )


# Non-source directories are omitted from temporary copies and symlink checks.
# Virtual environments and build caches often contain links to system files; scanning
# them would incorrectly classify an otherwise safe project as unsafe.
_NON_SOURCE_DIRS = frozenset(
    {
        "build",
        "build_out",
        "build_device",
        "build_host",
        "output",
        "out",
        "logs",
        "probe_config",
        "_CPack_Packages",
        "__pycache__",
        ".venv",
        "venv",
        ".git",
        "node_modules",
    }
)


def _check_case_dir_safe(case_dir: Path) -> None:
    """拒绝 case_dir 含指向外部的符号链接（防 sandbox 越狱 / 任意文件读）。

    跳过 ``_NON_SOURCE_DIRS``（构建产物 / 安装缓存 / venv）——它们不进 workspace 副本，
    其内部的 venv 符号链接（指向系统 Python）不应触发 case_unsafe。
    """
    case_real = case_dir.resolve()
    for root, dirs, files in os.walk(str(case_dir), followlinks=False):
        # Prune non-source directories before inspecting links.
        dirs[:] = [d for d in dirs if d not in _NON_SOURCE_DIRS]
        for name in list(dirs) + list(files):
            p = Path(root) / name
            try:
                is_link = p.is_symlink()
            except OSError:
                continue
            if not is_link:
                continue
            if p.is_dir():
                raise PatchValidationError(
                    f"directory symlink cannot be copied safely: {p}"
                )
            try:
                target = p.resolve(strict=False)
            except OSError as exc:
                raise PatchValidationError(
                    f"cannot resolve symlink in case_dir: {p} ({exc})"
                ) from exc
            try:
                target.relative_to(case_real)
            except ValueError as exc:
                raise PatchValidationError(
                    f"case_dir contains symlink escaping root: {p} -> {target}"
                ) from exc


def _build_sandbox_cmd(cmd: list[str], *, enable: bool) -> tuple[list[str], str]:
    """根据 enable + 可用工具包装命令，返回 (cmd, sandbox_id)。

    优先级：firejail > unshare -rn（用户+网络 namespace，不需 root）> 原样。
    sandbox_id 写入 verify.json 便于审计。
    """
    if not enable:
        return cmd, "off"
    if shutil.which("firejail"):
        return (
            ["firejail", "--quiet", "--net=none", "--private-tmp", "--"] + cmd,
            "firejail",
        )
    if shutil.which("unshare"):
        return (["unshare", "-rn", "--"] + cmd, "unshare-rn")
    return cmd, "unavailable"


def _normalize_patch_paths(text: str, source_case: Path, workspace_case: Path) -> str:
    normalized_lines: list[str] = []
    source_root = str(source_case.resolve())
    workspace_root = str(workspace_case.resolve())
    for line in text.splitlines(keepends=True):
        if line.startswith(("--- ", "+++ ")):
            prefix = line[:4]
            rest = line[4:].rstrip("\n")
            suffix = "\n" if line.endswith("\n") else ""
            if "\t" in rest:
                path_part, meta = rest.split("\t", 1)
                meta = "\t" + meta
            else:
                path_part, meta = rest, ""

            # Normalize path separators before stripping the project-root prefix.
            def _strip_root(p: str, root: str) -> str | None:
                for sep in ("/", "\\", os.sep):
                    if p.startswith(root + sep):
                        return p[len(root) + 1 :]
                return None

            stripped_root = _strip_root(path_part, source_root)
            if stripped_root is None:
                stripped_root = _strip_root(path_part, workspace_root)
            if stripped_root is not None:
                path_part = stripped_root
            elif path_part.startswith(str(PROJECT_ROOT) + "/") or path_part.startswith(
                str(PROJECT_ROOT) + "\\"
            ):
                path = Path(path_part)
                try:
                    path_part = str(path.resolve().relative_to(source_case.resolve()))
                except Exception:
                    pass

            path_part = path_part.replace("\\", "/")

            # Strip the optional a/ and b/ prefixes used by unified diffs.

            if path_part[:2] in ("a/", "b/"):
                stripped = path_part[2:]
                if stripped and (workspace_case / stripped).exists():
                    path_part = stripped
            normalized_lines.append(prefix + path_part + meta + suffix)
        else:
            normalized_lines.append(line)
    return "".join(normalized_lines)


_CATLASS_SRC_SUFFIXES = (".cpp", ".hpp", ".h", ".cce", ".cc")


def _resolve_catlass_example(case_dir: Path) -> tuple[str | None, str]:
    """Resolve the selected framework example from manifest metadata or environment."""
    mf = case_dir / "manifest.json"
    if mf.is_file():
        try:
            data = json.loads(mf.read_text(encoding="utf-8", errors="replace"))
        except (OSError, ValueError):
            data = {}
        sub = data.get("substrate")
        if sub:
            return str(sub), "manifest"
    env_ex = os.environ.get("ASCENDC_CATLASS_EXAMPLE")
    if env_ex:
        return env_ex, "env"
    return None, "none"


def _resolve_catlass_arch(case_dir: Path) -> tuple[str, str]:
    """Resolve the framework build architecture from manifest metadata or environment."""
    mf = case_dir / "manifest.json"
    if mf.is_file():
        try:
            data = json.loads(mf.read_text(encoding="utf-8", errors="replace"))
        except (OSError, ValueError):
            data = {}
        arch = data.get("arch")
        if arch:
            return str(arch), "manifest"
    env_arch = os.environ.get("ASCENDC_CATLASS_ARCH")
    if env_arch:
        return env_arch, "env"
    return "2201", "default"


def _catlass_tree_shape(root: Path) -> bool:
    return (root / "scripts" / "build.sh").is_file() and (root / "examples").is_dir()


def _full_catlass_case(case_dir: Path, example: str | None) -> bool:
    if not example or not _catlass_tree_shape(case_dir):
        return False
    return _owned_subpath(case_dir / "examples", example).is_dir()


def _catlass_overlay_tree_conflict(source: Path) -> Path | None:
    """A framework tree is not a source overlay, irrespective of its directory name."""
    for directory, dirs, _ in os.walk(source, followlinks=False):
        dirs[:] = [name for name in dirs if name not in _NON_SOURCE_DIRS]
        candidate = Path(directory)
        if _catlass_tree_shape(candidate):
            return candidate
    return None


def _is_catlass_case(case_dir: Path) -> bool:
    """Identify framework cases that require a source-overlay build."""
    example, _ = _resolve_catlass_example(case_dir)
    if _full_catlass_case(case_dir, example):
        return True
    if not os.environ.get("ASCENDC_CATLASS_ROOT"):
        return False
    mf = case_dir / "manifest.json"
    if mf.is_file():
        try:
            data = json.loads(mf.read_text(encoding="utf-8", errors="replace"))
        except (OSError, ValueError):
            data = {}
        if data.get("build_method") == "catlass_tree_overlay":
            return True
    for f in case_dir.rglob("*"):
        if f.is_file() and f.suffix in _CATLASS_SRC_SUFFIXES:
            try:
                head = f.read_text(encoding="utf-8", errors="replace")[:4000]
            except OSError:
                continue
            if '#include "catlass/' in head or "#include <catlass/" in head:
                return True
    return False


def _invocation_build_target(cmd: list[str]) -> str | None:
    """Read wrapper options without interpreting build.sh's original arguments."""
    start = next(
        (index for index, arg in enumerate(cmd) if Path(arg).name == "build_one.sh"),
        0,
    )
    for index in range(start, len(cmd)):
        arg = cmd[index]
        if arg == "--":
            break
        if arg.startswith("--target="):
            return arg.split("=", 1)[1]
        if arg == "--target" and index + 1 < len(cmd):
            return cmd[index + 1]
    return None


def _execute_build(
    cmd: list[str],
    cwd: Path,
    log_path: Path,
    timeout: int,
    project: Path,
    env: dict[str, str] | None = None,
) -> dict[str, Any]:
    """Execute one original recipe and keep partial logs and physical evidence."""
    artifact_before = object_state(cwd)
    cache_path = log_path.parent / "compilation_cache.json"
    cache = load_json(cache_path, default={}) or {}
    started_ns = time.time_ns()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    trace_path = log_path.with_suffix(".exec.trace")
    receipt_root = log_path.parent / "compiler_receipts"
    env = instrument_cmake(dict(env or os.environ), receipt_root)
    actual_cmd, tracing = trace_command(cmd, trace_path)
    if os.name == "posix" and shutil.which("timeout"):
        actual_cmd = [
            "timeout",
            "--signal=TERM",
            "--kill-after=5s",
            str(timeout),
            *actual_cmd,
        ]
    result = {
        "log_path": str(log_path),
        "sandbox": None,
        "recipe_argv": cmd,
        "recipe_cwd": str(cwd),
        "started_ns": started_ns,
        "compile_source_root": str(project),
        "build_root": str(cwd),
        "execution_tracing": tracing,
    }
    initial = failure_evidence(project, {}, status="unknown")
    required, scope = _configured_kernel_sources(
        cwd, project, _invocation_build_target(cmd)
    )
    context = {
        **result,
        "artifact_before": artifact_before,
        "cache": cache,
        "cache_path": str(cache_path),
        "trace_path": str(trace_path),
        "receipt_root": str(receipt_root),
        "initial": initial,
        "required_sources_before": [str(p) for p in required],
        "source_scope_before": scope,
        "invocation_target": _invocation_build_target(cmd),
        "actual_argv": actual_cmd,
        "timeout": timeout + 10,
    }
    context_path = recovery.prepare_stage(log_path, context)
    execution = execute_recipe(
        actual_cmd,
        cwd,
        log_path,
        timeout=timeout + 10,
        env=env,
        source_identity=initial["evidence_id"],
        verification_context=context_path,
    )
    result = _collect_build_result(context, execution, update_cache=True)
    result["recovery_stage"] = str(context_path)
    recovery.save(context_path.with_name("result.json"), result)
    return result


def _collect_build_result(
    context: dict, execution: dict, *, update_cache: bool = False
) -> dict:
    """Pure post-build reduction, shared by normal execution and recovery."""
    cwd, project = Path(context["build_root"]), Path(context["compile_source_root"])
    log_path = Path(context["log_path"])
    cache_path = Path(context["cache_path"])
    cache, artifact_before = context["cache"], context["artifact_before"]
    started_ns = context["started_ns"]
    trace_path, receipt_root = (
        Path(context["trace_path"]),
        Path(context["receipt_root"]),
    )
    tracing = context["execution_tracing"]
    result = {
        key: context[key]
        for key in (
            "log_path",
            "sandbox",
            "recipe_argv",
            "recipe_cwd",
            "started_ns",
            "compile_source_root",
            "build_root",
            "execution_tracing",
        )
    }
    result.update(execution)
    if execution.get("status") in {"unknown", "running"}:
        result["verify_infra_error"] = "execution_unconfirmed"
    result["execution_receipt"] = (
        str(Path(execution["run_dir"]) / "result.json")
        if execution.get("run_dir")
        else None
    )
    log_path = Path(execution.get("log_path") or log_path)
    output = read_text(log_path)
    if execution.get("returncode") == 124:
        result.update(status="interrupted", interruption_reason="timeout")
    normalized = normalize_log_text(output)
    result["signature"] = normalized.get("normalized_signature")
    result["ended_ns"] = execution.get("ended_ns", time.time_ns())
    result["failure_evidence"] = failure_evidence(
        project, normalized, status=result["status"]
    )
    previous_records = (
        cache.get("records", [])
        if cache.get("recipe_sha256") == result["failure_evidence"]["recipe_sha256"]
        else []
    )
    # Invocation-level build target (``--target X`` on the recipe command): attributed to
    # compilation records whose object path carries no direct CMakeFiles/<target>.dir
    # segment (vendor-framework kernel objects) — see collect_compilation_evidence.
    invocation_target = context["invocation_target"]
    required_sources, source_scope = _configured_kernel_sources(
        cwd, project, invocation_target
    )
    result["compilation_evidence"] = collect_compilation_evidence(
        cwd,
        project,
        started_ns,
        required_sources,
        artifact_before=artifact_before,
        previous_records=previous_records,
        execution_receipts=(
            read_receipts(trace_path, cwd) if tracing["status"] == "enabled" else []
        )
        + compiler_receipts(receipt_root, started_ns),
        build_target=invocation_target,
    )
    # A configured pre-build obligation cannot shrink during collection.
    if context["source_scope_before"].get("status") == "configured":
        frozen = set(context["required_sources_before"])
        if not frozen.issubset(str(p) for p in required_sources):
            result["compilation_evidence"]["status"] = "unconfirmed"
            result["compilation_evidence"]["obligation_gap"] = (
                "configured_sources_removed"
            )
        result["compilation_evidence"]["required_sources"] = sorted(
            frozen | set(result["compilation_evidence"].get("required_sources", []))
        )
    result["compilation_evidence"]["source_scope"] = source_scope
    if update_cache:
        write_json(
            cache_path,
            {
                "recipe_sha256": result["failure_evidence"]["recipe_sha256"],
                "records": result["compilation_evidence"]["records"],
            },
        )
    return result


def recover_build(context_path: Path) -> dict:
    """Collect a dispatched stage, with no recipe, patch or retry execution."""
    context = recovery.read(context_path)
    binding = recovery.read(context_path.with_name("execution.json"))
    run_dir = Path(binding["run_dir"])
    execution = observe_run(run_dir, expected_identity=binding["identity"])
    gaps = []
    if (
        binding.get("argv") != context["actual_argv"]
        or binding.get("cwd") != str(Path(context["recipe_cwd"]).resolve())
        or binding.get("source") != context["initial"]["evidence_id"]
        or binding.get("verification_context") != str(context_path)
        or execution.get("run_id") != binding.get("run_id")
    ):
        gaps.append("execution_binding_changed")
    if (
        execution.get("status") not in {"passed", "failed", "interrupted"}
        or execution.get("log_capture") != "complete"
    ):
        gaps.append(execution.get("reason") or "execution_not_terminal")
    witness_ref = execution.get("verification_witness") or {}
    witness_path = context_path.with_name("witness.json")
    if witness_ref.get("path") != str(witness_path) or witness_ref.get(
        "sha256"
    ) != recovery.file_hash(witness_path):
        gaps.append("terminal_witness_missing_or_changed")
    else:
        witness = recovery.read(witness_path)
        if witness.get("context_sha256") != digest(context):
            gaps.append("witness_context_changed")
        if witness.get("evidence", {}).get("status") != "complete":
            gaps.append("terminal_evidence_incomplete")
        elif recovery.evidence_snapshot(context) != witness["evidence"]:
            gaps.append("build_evidence_changed")
        before_files = context["initial"]["source_files"]
        after_files = witness["source"]["source_files"]
        if any(after_files.get(k) != value for k, value in before_files.items()):
            gaps.append("source_changed_during_build")
    if context["tool_identity"] != recovery.tool_identity():
        gaps.append("verifier_changed")
    if context["initial"].get("unreadable"):
        gaps.append("original_source_unreadable")
    if context["environment_identity"] != recovery.environment_identity():
        gaps.append("environment_changed")
    if gaps:
        return {
            "status": "incomplete",
            "gaps": sorted(set(gaps)),
            "execution": execution,
            "new_build_invoked": False,
        }
    result = _collect_build_result(context, execution)
    result["recovery_stage"] = str(context_path)
    result = _finish_adapter(result, context["adapter"])
    return {"status": "collected", "build": result, "new_build_invoked": False}


def _recovered_impl_result(state: dict, build: dict) -> dict:
    edge = state["edge"]
    before = edge.get("before") if edge["label"] == "after" else build
    mode = edge["mode"]
    after = (
        build
        if edge["label"] == "after" or mode == "in_place"
        else {"status": "skipped"}
    )
    result = {
        **{
            k: edge[k]
            for k in (
                "soc_used",
                "soc_source",
                "soc_requested",
                "soc_declared",
                "soc_configuration_conflict",
                "workspace",
            )
        },
        "before": before,
        "after": after,
        "build_invoked": bool(build.get("build_invoked")),
        "dry_run": False,
        "verify_mode": "in_place" if mode == "in_place" else "patch",
        "transition": _compute_transition(before, after),
        "patch_apply": edge.get("patch_apply")
        or {
            "status": "not_applicable" if mode == "in_place" else "skipped",
            "dry_run_ok": None,
            "log_path": None,
        },
        "logs": {"before": before.get("log_path"), "after": after.get("log_path")},
    }
    if build.get("verify_infra_error"):
        result.update(
            transition="environment_unavailable",
            verify_infra_error=build["verify_infra_error"],
        )
    return result


def recover_verification(directory: Path) -> dict:
    """Recompute acceptance from frozen obligations. Never calls verify/build."""
    directory = directory.resolve()
    result = {
        "transition": "verification_incomplete",
        "build_invoked": None,
        "validation_scope": {
            "compile": "unconfirmed",
            "contract": "not_checked",
            "numerical": "not_checked",
        },
        "recovery": {
            "directory": str(directory),
            "status": "incomplete",
            "new_build_invoked": False,
            "gaps": [],
        },
    }
    gaps = result["recovery"]["gaps"]
    try:
        state = recovery.read(directory / "session.json")
        for name in ("result.json", "recovered-result.json"):
            if (directory / name).is_file():
                result["historical_result"] = recovery.read(directory / name)
                break
        if not state.get("owner_finished") and _alive(state["owner"]) is not False:
            gaps.append("verification_owner_active_or_unknown")
            return result
        request = state["request"]
        if recovery.input_state(Path(request["case_dir"])) != state["input"]:
            gaps.append("original_input_changed")
        if state["input"].get("unreadable"):
            gaps.append("original_input_unreadable")
        if any(
            recovery.file_hash(Path(p)) != value
            for p, value in request["files"].items()
        ):
            gaps.append("request_file_changed")
        if recovery.tool_identity() != state["tool_identity"]:
            gaps.append("verifier_changed")
        if recovery.environment_identity() != state["environment_identity"]:
            gaps.append("environment_changed")
        stage_path = state.get("active_stage")
        if not stage_path:
            gaps.append("stage_not_dispatched_or_not_journaled")
            return result
        collected = recover_build(Path(stage_path))
        result["stage_observation"] = collected
        result["build_invoked"] = (
            collected.get("build") or collected.get("execution") or {}
        ).get("build_invoked")
        if collected["status"] != "collected":
            gaps.extend(collected["gaps"])
            return result
        edge = state.get("edge") or {}
        if edge.get("mode") != "in_place" and edge.get("label") != "after":
            gaps.append("patch_or_after_stage_not_completed")
        # A dependency-race retry is an authorized live action, never a recovery action.
        build = collected["build"]
        context = recovery.read(Path(stage_path))
        if _is_dependency_race_failure(
            normalize_log_text(read_text(Path(build["log_path"]))),
            build.get("returncode"),
        ) and not Path(context["log_path"]).stem.endswith("_retry"):
            gaps.append("live_retry_decision_not_completed")
        if gaps:
            return result
        raw = state.get("raw_result")
        if raw:
            raw = json.loads(json.dumps(raw))
            if edge["mode"] == "in_place":
                raw["before"] = raw["after"] = build
            else:
                raw["after"] = build
        else:
            raw = _recovered_impl_result(state, build)
        final = _finalize_verify_result(
            raw,
            Path(request["case_dir"]),
            **state["resolved"],
            previous_snapshot=request.get("previous_snapshot"),
        )
        final["recovery"] = {
            "directory": str(directory),
            "status": "collected",
            "new_build_invoked": False,
            "gaps": [],
            "current_applicability": "matched",
        }
        recovery.save(directory / "recovered-result.json", final)
        return final
    except (OSError, ValueError, TypeError, KeyError) as exc:
        gaps.append("checkpoint_unavailable_or_invalid")
        result["recovery"]["error_type"] = type(exc).__name__
        return result


def _execute_adapter(metadata: dict, runner, *args):
    with recovery.adapter(metadata):
        result = runner(*args)
    return _finish_adapter(result, metadata)


def _finish_adapter(result: dict, metadata: dict) -> dict:
    kind = metadata.get("kind")
    fields = {k: v for k, v in metadata.items() if k not in {"kind", "full_root"}}
    result = {**fields, **result}
    if kind == "ordinary":
        result.update(
            sandbox=metadata["sandbox"], expected_target=metadata["expected_target"]
        )
        if result["status"] == "failed":
            infra = _classify_build_env_failure(
                result["returncode"], read_text(Path(result["log_path"]))
            )
            if infra:
                result["verify_infra_error"] = infra
    if kind == "catlass" and metadata.get("full_root"):
        root = Path(metadata["full_root"])
        result["failure_evidence"] = failure_evidence(
            root,
            normalize_log_text(read_text(Path(result["log_path"]))),
            status=result["status"],
        )
        result["compile_source_root"] = str(root)
        compilation = result.get("compilation_evidence")
        if compilation is not None:
            compilation["project_root"] = str(root)
            for record in compilation.get("records", []):
                record["dependencies"] = sorted(
                    str(Path(path).resolve())
                    for path in record.get("dependency_sha256", {})
                    if Path(path).resolve().is_relative_to(root)
                )
    return result


def _execute_with_race_retry(
    cmd: list[str],
    cwd: Path,
    log_path: Path,
    timeout: int,
    project: Path,
    env: dict[str, str] | None = None,
) -> dict:
    result = _execute_build(cmd, cwd, log_path, timeout, project, env)
    normalized = normalize_log_text(read_text(Path(result.get("log_path") or log_path)))
    if _is_dependency_race_failure(normalized, result.get("returncode")):
        retry_env = dict(env or os.environ)
        retry_env["MAKEFLAGS"] = "-j1"
        retry_log = log_path.with_name(log_path.stem + "_retry.log")
        retry = _execute_build(cmd, cwd, retry_log, timeout, project, retry_env)
        retry["build_retry"] = {
            "reason": "dependency_file_race",
            "attempt": 2,
            "first_returncode": result["returncode"],
            "first_log_path": result["log_path"],
        }
        return retry
    return result


def _run_build_catlass(
    case_dir: Path, label: str, extra_env: dict | None = None, soc: str | None = None
) -> dict[str, Any]:
    """Build the selected example in a private full tree or private example overlay."""
    example, example_source = _resolve_catlass_example(case_dir)
    full_root = _full_catlass_case(case_dir, example)
    source_root = (
        case_dir.resolve()
        if full_root
        else Path(os.environ["ASCENDC_CATLASS_ROOT"]).resolve()
    )
    catlass_root = source_root

    arch, arch_source = _resolve_catlass_arch(case_dir)
    if soc:
        arch, arch_source = ("3510" if "950" in soc else "2201"), "resolved_soc"
    build_sh = catlass_root / "scripts" / "build.sh"
    log_path = case_dir / "logs" / f"verify_{label}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    if not example:
        # Without an explicit example, the overlay target is ambiguous.
        return {
            "status": "failed",
            "signature": None,
            "returncode": None,
            "log_path": str(log_path),
            "sandbox": None,
            "verify_infra_error": "catlass_example_unresolved",
            "built_target": None,
            "expected_target": None,
            "example_source": example_source,
        }
    ex_dir = _owned_subpath(catlass_root / "examples", example)
    if not build_sh.exists() or not ex_dir.is_dir():
        return {
            "status": "failed",
            "signature": None,
            "returncode": None,
            "log_path": str(log_path),
            "sandbox": None,
            "verify_infra_error": "catlass_tree_missing",
            "built_target": None,
            "expected_target": example,
            "example_source": example_source,
        }
    if not full_root:
        overlay_root = (
            case_dir / "project" if (case_dir / "project").is_dir() else case_dir
        )
        conflict = _catlass_overlay_tree_conflict(overlay_root)
        if conflict is not None:
            return {
                "status": "not_started",
                "signature": None,
                "returncode": None,
                "log_path": None,
                "build_invoked": False,
                "verify_infra_error": "ambiguous_framework_overlay_source",
                "framework_source_conflict": str(conflict),
                "built_target": None,
                "expected_target": example,
                "example_source": example_source,
            }
    # Each verification owns its framework copy; sharing and later restoring a
    # tree cannot protect concurrent builds or a process terminated during a build.
    catlass_root = _private_framework_tree(case_dir, source_root, phase=label)
    build_sh = catlass_root / "scripts" / "build.sh"
    ex_dir = _owned_subpath(catlass_root / "examples", example)
    if not full_root:
        _overlay_case_sources(case_dir, ex_dir)
    try:
        env = os.environ.copy()
        if extra_env:
            env.update(extra_env)
        metadata = {
            "kind": "catlass",
            "full_root": str(catlass_root) if full_root else None,
            "build_method": "catlass_full_tree"
            if full_root
            else "catlass_tree_overlay",
            "expected_target": example,
            "selected_example": example,
            "requested_arch": arch,
            "arch_source": arch_source,
            "example_source": example_source,
        }
        return _execute_adapter(
            metadata,
            _execute_with_race_retry,
            ["bash", str(build_sh), "--clean", f"-DCATLASS_ARCH={arch}", example],
            catlass_root,
            log_path,
            1800,
            ex_dir,
            env,
        )

    finally:
        # Keep this owned snapshot intact: receipts and dependency paths remain auditable.
        pass


# Framework adapters select source overlays and recipe modes from manifest metadata.
_CANNOPS_ARCH_TO_SOC = {"2201": "ascend910b", "3510": "ascend950"}
_CANNOPS_SRC_SUFFIXES = {".asc", ".cce", ".cpp", ".cc", ".cxx", ".h", ".hpp", ".hh"}
_CANNOPS_KEEP_NAMES = {"CMakeLists.txt"}


def _manifest_json(case_dir: Path) -> dict[str, Any]:
    """case manifest 的小读取器（缺文件/坏 JSON → 空 dict，不抛）。"""
    mf = case_dir / "manifest.json"
    if not mf.is_file():
        return {}
    try:
        return json.loads(mf.read_text(encoding="utf-8", errors="replace"))
    except (OSError, ValueError):
        return {}


def _owned_subpath(root: Path, relative: str) -> Path:
    target = (root / relative).resolve()
    if not relative or target == root.resolve() or root.resolve() not in target.parents:
        raise PatchValidationError(
            f"framework path must stay inside its root: {relative!r}"
        )
    return target


def _overlay_case_sources(
    case_dir: Path, target_dir: Path
) -> tuple[dict[Path, str], list[Path]]:
    """快照 target_dir 源文件 → 把 case 源树（project/ 优先）按相对路径覆盖/注入。

    覆盖是一次性的：本阶段持有该框架快照作为构建证据（receipt 与依赖路径
    的可审计性要求保留覆盖后的树），不做恢复。只动源码后缀与
    CMakeLists.txt——manifest/logs/产物不进工作树。
    """

    def _eligible(p: Path) -> bool:
        return p.is_file() and (
            p.suffix in _CANNOPS_SRC_SUFFIXES or p.name in _CANNOPS_KEEP_NAMES
        )

    snapshot = {
        f: f.read_text(encoding="utf-8", errors="replace")
        for f in target_dir.rglob("*")
        if _eligible(f)
    }
    src_root = case_dir / "project" if (case_dir / "project").is_dir() else case_dir
    added: list[Path] = []
    for sf in src_root.rglob("*"):
        if not _eligible(sf):
            continue
        rel = sf.relative_to(src_root)
        if any(part in _NON_SOURCE_DIRS for part in rel.parts):
            continue
        target = target_dir / rel
        if target in snapshot or target.exists():
            target.write_text(
                sf.read_text(encoding="utf-8", errors="replace"), encoding="utf-8"
            )
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(
                sf.read_text(encoding="utf-8", errors="replace"), encoding="utf-8"
            )
            added.append(target)
    return snapshot, added


def _cannops_base_result(
    case_dir: Path,
    label: str,
    m: dict[str, Any],
    arch: str,
    arch_source: str,
    mode: str,
) -> dict[str, Any]:
    op = str(
        m.get("op")
        or m.get("operator")
        or Path(str(m.get("substrate_path", ""))).name
        or "?"
    )
    log_path = case_dir / "logs" / f"verify_{label}.log"
    return {
        "log_path": str(log_path),
        "sandbox": None,
        "build_method": "cannops_repo_build",
        "built_target": op,
        "expected_target": op,
        "built_arch": arch,
        "arch_source": arch_source,
        "recipe": f"cannops_{mode}",
        "soc": _CANNOPS_ARCH_TO_SOC.get(str(arch), "ascend910b"),
    }


def _run_build_cannops(
    case_dir: Path, label: str, extra_env: dict | None = None, soc: str | None = None
) -> dict[str, Any]:
    """Build a cann-ops custom-operator case with the project's build script."""
    m = _manifest_json(case_dir)
    arch, arch_source = _resolve_catlass_arch(case_dir)
    if soc:
        arch, arch_source = ("3510" if "950" in soc else "2201"), "resolved_soc"
    mode = str(m.get("mode") or "plain")
    base = _cannops_base_result(case_dir, label, m, arch, arch_source, mode)
    root_env = os.environ.get("ASCENDC_CANNOPS_ROOT")
    if not root_env:
        return {
            **base,
            "status": "failed",
            "signature": None,
            "returncode": None,
            "verify_infra_error": "cannops_root_unconfigured",
        }
    repo = Path(root_env)
    if m.get("substrate_repo"):
        repo = _owned_subpath(repo, str(m["substrate_repo"]))
    op_dir = _owned_subpath(repo, str(m.get("substrate_path", "")))
    build_sh = repo / "build.sh"
    if not op_dir.is_dir() or not build_sh.is_file():
        return {
            **base,
            "status": "failed",
            "signature": None,
            "returncode": None,
            "verify_infra_error": "cannops_repo_layout_missing",
        }
    repo = _private_framework_tree(case_dir, repo, phase=label)
    op_dir = _owned_subpath(repo, str(m.get("substrate_path", "")))
    build_sh = repo / "build.sh"
    log_path = Path(base["log_path"])
    log_path.parent.mkdir(parents=True, exist_ok=True)
    op = base["built_target"]
    soc = base["soc"]
    cmd = ["bash", str(build_sh)]
    if mode == "pkg":
        cmd.append("--pkg")
    elif mode == "nopkg":
        cmd.append(
            "--noaclnn"
        )  # The --noaclnn option belongs to ops-nn; transformer uses plain/pkg mode.
    cmd += [f"--soc={soc}", f"--ops={op}", "-j4"]
    tmo = int(m.get("build_timeout") or 2700)
    # 覆盖后的框架树由本阶段持有，作为构建证据保留（见 _overlay_case_sources）。
    _overlay_case_sources(case_dir, op_dir)
    env = dict(os.environ)
    if extra_env:
        env.update(extra_env)
    base.pop("built_arch", None)
    base.pop("built_target", None)
    return _execute_adapter(
        {"kind": "cannops", **base, "requested_arch": arch},
        _execute_build,
        cmd,
        repo,
        log_path,
        tmo,
        op_dir,
        env,
    )


def _hccl_device_salvage(repo: Path, tmo: int) -> tuple[int, str]:
    """Use the documented fallback for environments where the device library is unavailable."""
    try:
        env_ver = (detect_cann() or {}).get("version")
    except Exception:
        env_ver = None
    vc = classify_version(None, env_ver)
    detected_dom = normalize_version(env_ver)
    floor_dom = normalize_version(vc.get("kb_floor"))
    if detected_dom != floor_dom:
        return 1, (
            f"[hccl-device-salvage] skipped: 9.0.0 专属补救按版本域门拦截"
            f"（detected={env_ver!r}, resolution={vc.get('resolution')!r}）"
            "——仅在 floor 域执行，其余域未核实不套用。"
        )
    bdir = repo / "build_device"
    if not bdir.is_dir():
        return 1, ""
    try:
        c = subprocess.run(
            ["cmake", "-DPRODUCT_SIDE=device", "."],
            cwd=str(bdir),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=600,
        )
        mk = subprocess.run(
            ["make", "custom_aicpu", "-j4"],
            cwd=str(bdir),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=tmo,
        )
        return mk.returncode, c.stdout + "\n" + mk.stdout
    except (subprocess.TimeoutExpired, OSError):
        return 1, ""


def _run_build_hccl(
    case_dir: Path, label: str, extra_env: dict | None = None
) -> dict[str, Any]:
    """Build an hccl custom-operator case with the project's build script."""
    m = _manifest_json(case_dir)
    example = str(m.get("hccl_example", ""))
    ops = str(m.get("op") or "")
    log_path = case_dir / "logs" / f"verify_{label}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    base: dict[str, Any] = {
        "log_path": str(log_path),
        "sandbox": None,
        "build_method": "hccl_custom_ops",
        "built_target": ops or example,
        "expected_target": ops or example,
        "built_arch": None,
        "arch_source": "na",
        "recipe": "hccl_custom",
    }
    root_env = os.environ.get("ASCENDC_HCCL_ROOT")
    if not root_env:
        return {
            **base,
            "status": "failed",
            "signature": None,
            "returncode": None,
            "verify_infra_error": "hccl_root_unconfigured",
        }
    repo = Path(root_env)
    ex_dir = _owned_subpath(repo, example) if example else None
    if ex_dir is None or not ex_dir.is_dir() or not (repo / "build.sh").is_file():
        return {
            **base,
            "status": "failed",
            "signature": None,
            "returncode": None,
            "verify_infra_error": "hccl_example_dir_missing",
        }
    repo = _private_framework_tree(case_dir, repo, phase=label)
    ex_dir = _owned_subpath(repo, example)
    cmd = [
        "bash",
        str(repo / "build.sh"),
        "--vendor=cust",
        f"--ops={ops}",
        f"--custom_ops_path=./{example}",
    ]
    tmo = int(m.get("build_timeout") or 1800)
    # 覆盖后的框架树由本阶段持有，作为构建证据保留（见 _overlay_case_sources）。
    _overlay_case_sources(case_dir, ex_dir)
    env = dict(os.environ)
    if extra_env:
        env.update(extra_env)
    # A verifier must not silently replace a failed recipe with stub libraries.
    base.pop("built_target", None)
    return _execute_adapter(
        {"kind": "hccl", **base}, _execute_build, cmd, repo, log_path, tmo, ex_dir, env
    )


# The build banner distinguishes a started recipe from shell setup failure.
_BUILD_HARNESS_BANNER = "=== SOC_VERSION="


def _classify_build_env_failure(returncode: int | None, log_text: str) -> str | None:
    """Classify a build that did not reach a usable compiler as an environment failure."""
    if returncode == 0:
        return None
    text = log_text or ""
    if _BUILD_HARNESS_BANNER not in text:
        return "build_harness_not_started"
    if returncode in (126, 127) or "command not found" in text:
        return "build_toolchain_missing"
    return None


# Dependency-file creation failures may be transient. Retry only when every
# reported error has that shape, so source or linker failures are not hidden.
_DEPENDENCY_RACE_RE = re.compile(r"error opening '.*\.o(\.d)?': No such file")


def _is_dependency_race_failure(
    normalized: dict[str, Any], returncode: int | None
) -> bool:
    """构建失败是否**纯**死于并行依赖文件竞态（可安全重试）。

    判据（三全）：rc != 0；日志里有竞态形态行；all_errors 里**每一条**都是该形态
    （无任何源码级/链接级错误——有真源码错的失败绝不重试，避免掩盖真回归）。
    """
    if returncode == 0:
        return False
    errors = normalized.get("all_errors") or []
    if not errors:
        return False
    for err in errors:
        blob = f"{err.get('message') or ''}\n{err.get('raw') or ''}"
        if not _DEPENDENCY_RACE_RE.search(blob):
            return False
    return any(
        _DEPENDENCY_RACE_RE.search(f"{e.get('message') or ''}\n{e.get('raw') or ''}")
        for e in errors
    )


def _run_build(
    case_dir: Path,
    label: str,
    soc: str | None = None,
    sandbox: bool = False,
    extra_env: dict[str, str] | None = None,
    build_args: list[str] | None = None,
    build_target: str | None = None,
    cmake_args: list[str] | None = None,
) -> dict[str, Any]:
    if (build_args or build_target or cmake_args) and (
        _is_catlass_case(case_dir)
        or _manifest_json(case_dir).get("build_method")
        in {"cannops_repo_build", "hccl_custom_ops"}
    ):
        return {
            "status": "not_started",
            "returncode": None,
            "build_invoked": False,
            "verify_infra_error": "recipe_options_unsupported_for_framework_adapter",
        }
    if _is_catlass_case(case_dir):
        return _run_build_catlass(case_dir, label, extra_env, soc=soc)
    method = _manifest_json(case_dir).get("build_method")
    if method == "cannops_repo_build":
        return _run_build_cannops(case_dir, label, extra_env, soc=soc)
    if method == "hccl_custom_ops":
        return _run_build_hccl(case_dir, label, extra_env)
    script = PROJECT_ROOT / "harness/runtime/build_one.sh"
    project = case_dir / "project" if (case_dir / "project").is_dir() else case_dir
    soc = soc or read_project_soc_version(project)
    if not soc:
        return {
            "status": "not_started",
            "build_invoked": False,
            "returncode": None,
            "log_path": None,
            "verify_infra_error": "requested_soc_unknown",
        }
    log_path = case_dir / "logs" / f"verify_{label}.log"
    cmd = ["bash", str(script), str(project), soc]
    if build_target:
        cmd += ["--target", build_target]
    for arg in cmake_args or []:
        cmd += ["--cmake-arg", arg]
    if build_args:
        cmd += ["--", *build_args]
    wrapped, sandbox_id = _build_sandbox_cmd(cmd, enable=sandbox)
    env = dict(os.environ)
    if extra_env:
        env.update(extra_env)
    return _execute_adapter(
        {"kind": "ordinary", "sandbox": sandbox_id, "expected_target": build_target},
        _execute_with_race_retry,
        wrapped,
        project,
        log_path,
        300,
        project,
        env,
    )


# Normalize text in the disposable copy so LF patches can match CRLF sources.
_LF_NORMALIZE_SUFFIXES = {
    ".asc",
    ".cpp",
    ".cc",
    ".cxx",
    ".c",
    ".h",
    ".hpp",
    ".json",
    ".txt",
    ".cmake",
    ".md",
    ".sh",
    ".py",
    ".patch",
    ".ini",
}


def _normalize_text_lf(root: Path) -> int:
    """把 root 下文本文件的 CRLF 归一为 LF；返回转换文件数。"""
    converted = 0
    for p in root.rglob("*"):
        if not p.is_file():
            continue
        if p.suffix.lower() in _LF_NORMALIZE_SUFFIXES or p.name == "CMakeLists.txt":
            try:
                data = p.read_bytes()
            except OSError:
                continue
            if b"\r\n" in data:
                p.write_bytes(data.replace(b"\r\n", b"\n"))
                converted += 1
    return converted


def _workspace_root() -> Path:
    """Explicit workspace/scratch root, otherwise external temporary storage."""
    env = os.environ.get("ACD_WORKSPACE_ROOT") or os.environ.get("ACD_SCRATCH_ROOT")
    if env:
        return Path(env)
    return Path(tempfile.gettempdir()) / "acd_verify_work"


def _copy_case_to_workspace(case_dir: Path) -> Path:
    case_dir = case_dir.resolve()
    if not case_dir.is_dir():
        raise PatchValidationError("case_dir must be an existing directory")
    verify_root = (_workspace_root() / "workspace" / "builds" / "verify").resolve()
    if verify_root == case_dir or case_dir in verify_root.parents:
        raise PatchValidationError(
            "verification workspace overlaps or is inside the source project; "
            "choose an external ACD_WORKSPACE_ROOT/TMPDIR"
        )
    _check_case_dir_safe(case_dir)
    verify_root.mkdir(parents=True, exist_ok=True)
    run_root = Path(tempfile.mkdtemp(prefix="run-", dir=verify_root))
    workspace_case = run_root / case_dir.name
    ignore = shutil.ignore_patterns(*_NON_SOURCE_DIRS, "*.run")
    shutil.copytree(case_dir, workspace_case, ignore=ignore, symlinks=False)
    _normalize_text_lf(workspace_case)
    (workspace_case / "logs").mkdir(parents=True, exist_ok=True)
    return workspace_case


def _private_framework_tree(
    case_dir: Path, source_root: Path, *, phase: str = ""
) -> Path:
    """Clone framework sources once per verification; never mutate the user's tree."""
    source_root = source_root.resolve()
    key = hashlib.sha256((str(source_root) + "|" + phase).encode("utf-8")).hexdigest()[
        :16
    ]
    parent = (
        case_dir.parent / "frameworks"
        if source_root == case_dir.resolve()
        else case_dir / "logs" / "frameworks"
    )
    target = (parent / key).resolve()
    if target == source_root or source_root in target.parents:
        raise PatchValidationError("framework workspace is inside its source tree")
    marker = target.parent / f"{key}.json"
    if target.is_dir() and load_json(marker, default={}) == {
        "source": str(source_root),
        "owner": str(case_dir.resolve()),
    }:
        return target
    if target.exists():
        raise PatchValidationError(
            "framework workspace is not owned by this verification"
        )
    _check_case_dir_safe(source_root)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(
        source_root,
        target,
        ignore=shutil.ignore_patterns(*_NON_SOURCE_DIRS, "*.run"),
        symlinks=False,
    )
    write_json(marker, {"source": str(source_root), "owner": str(case_dir.resolve())})
    return target


def _patch_text_from_suggestions(path: Path) -> str:
    data = load_json(path, default={}) or {}
    suggestions = data.get("suggestions") or []
    for suggestion in suggestions:
        patch = suggestion.get("patch")
        if isinstance(patch, str) and patch.startswith("--- "):
            return patch
    return ""


def _materialize_patch(
    patch_file: Path | None,
    patch_suggestions: Path | None,
    workspace_case: Path,
    source_case: Path | None = None,
) -> Path | None:
    text = ""
    if patch_file and patch_file.exists():
        text = read_text(patch_file)
    if (
        (not text or not text.startswith("--- "))
        and patch_suggestions
        and patch_suggestions.exists()
    ):
        text = _patch_text_from_suggestions(patch_suggestions)
    if not text or not text.startswith("--- "):
        return None
    text = _normalize_patch_paths(text, source_case or workspace_case, workspace_case)
    _validate_patch_text(text)
    patch_path = workspace_case / "logs" / "verify_apply.patch"
    patch_path.write_text(text, encoding="utf-8")
    return patch_path


def _extract_patch_source_paths(patch_text: str) -> list[str]:
    """从 unified diff 头部提取 `--- ` 行的源文件路径（去掉 a/ 前缀，跳过 /dev/null）。"""
    paths: list[str] = []
    seen: set[str] = set()
    for line in patch_text.splitlines():
        if not line.startswith("--- "):
            continue
        rest = line[4:].split("\t", 1)[0].strip()
        if rest in ("", "-", "/dev/null"):
            continue
        if rest.startswith("a/"):
            rest = rest[2:]
        if rest in seen:
            continue
        seen.add(rest)
        paths.append(rest)
    return paths


def _check_patch_source_files_exist(
    workspace_case: Path, patch_path: Path
) -> list[str]:
    """返回 patch 引用但 workspace 内不存在的源文件相对路径列表。"""
    text = patch_path.read_text(encoding="utf-8", errors="replace")
    missing: list[str] = []
    for rel in _extract_patch_source_paths(text):
        target = workspace_case / rel
        if not target.exists():
            missing.append(rel)
    return missing


def _apply_patch(
    workspace_case: Path,
    patch_path: Path,
    dry_run_only: bool = False,
) -> dict[str, Any]:
    """Run a patch dry-run before applying it. When dry_run_only is true, validation is performed without changing the temporary copy."""
    log_path = workspace_case / "logs" / "verify_patch_apply.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    # Normalize a temporary patch copy to match the workspace's LF line endings.
    try:
        patch_bytes = Path(patch_path).read_bytes()
    except OSError:
        patch_bytes = b""
    if b"\r\n" in patch_bytes:
        normalized = workspace_case / "logs" / "patch.lf.patch"
        normalized.write_bytes(patch_bytes.replace(b"\r\n", b"\n"))
        patch_path = normalized
    dry = subprocess.run(
        ["patch", "--batch", "--dry-run", "-p0", "-i", str(patch_path)],
        cwd=workspace_case,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        timeout=60,
    )
    if dry.returncode != 0:
        log_path.write_text(
            "[dry-run failed; workspace untouched]\n" + dry.stdout,
            encoding="utf-8",
            errors="replace",
        )
        return {
            "status": "failed",
            "dry_run_ok": False,
            "returncode": dry.returncode,
            "log_path": str(log_path),
            "output_excerpt": "\n".join(dry.stdout.splitlines()[:30]),
        }
    if dry_run_only:
        log_path.write_text(
            "[dry-run only; workspace untouched]\n" + dry.stdout,
            encoding="utf-8",
            errors="replace",
        )
        return {
            "status": "passed",
            "dry_run_ok": True,
            "dry_run_only": True,
            "returncode": 0,
            "log_path": str(log_path),
            "output_excerpt": "\n".join(dry.stdout.splitlines()[:30]),
        }
    proc = subprocess.run(
        ["patch", "--batch", "-p0", "-i", str(patch_path)],
        cwd=workspace_case,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        timeout=60,
    )
    log_path.write_text(proc.stdout, encoding="utf-8", errors="replace")
    return {
        "status": "passed" if proc.returncode == 0 else "failed",
        "dry_run_ok": True,
        "returncode": proc.returncode,
        "log_path": str(log_path),
        "output_excerpt": "\n".join(proc.stdout.splitlines()[:30]),
    }


def _emit_verify_audit(event: str, payload: dict[str, Any]) -> dict[str, Any]:
    """Report audit persistence separately from the compilation outcome."""
    try:
        from harness.engine.audit import emit_audit

        return emit_audit(event, payload)
    except Exception as exc:
        import sys

        kind = type(exc).__name__
        sys.stderr.write(f"[audit] audit_hook_failed ({kind})\n")
        return {"status": "failed", "reason": "audit_hook_failed", "error_type": kind}


# Missing build prerequisites are infrastructure failures, not source failures.

_ENV_GATE_SKIP_VAR = "ACD_VERIFY_SKIP_ENV_CHECK"

_ENV_REASON_GUIDANCE: dict[str, dict[str, Any]] = {
    "cann_not_found": {
        "what": (
            "未找到 CANN 安装（查过 ASCEND_CANN_PACKAGE_PATH / ASCEND_TOOLKIT_HOME / "
            "ASCEND_HOME_PATH 环境变量与常见安装路径）。run_verify 需要真实编译环境"
            "（Linux/WSL2 + CANN toolkit）。"
        ),
        "fixes": [
            "换到装有 CANN 的 Linux/WSL2 环境后重跑 run_verify；",
            "若 CANN 装在非标准位置：设置 ASCEND_TOOLKIT_HOME 指向它，或由用户确认后设 "
            f"{_ENV_GATE_SKIP_VAR}=1 显式跳过本闸门重试；",
        ],
    },
    "cmake_not_found": {
        "what": "未找到 cmake（build_one.sh 依赖）。CANN 在但构建工具链不全。",
        "fixes": [
            "安装/加载 cmake（如 source CANN 的环境脚本）后重试；",
            f"确知 cmake 可用时（如仅 PATH 未透传）：设 {_ENV_GATE_SKIP_VAR}=1 显式跳过本闸门重试；",
        ],
    },
    "build_harness_not_started": {
        "what": (
            "构建脚本从未启动（build_one.sh 的环境横幅未出现在日志里；常见于 bash 不可用、"
            "WSL 未安装、case 目录不可进入）。"
        ),
        "fixes": [
            "读 logs.before 指向的日志尾部，确认具体是哪一类启动失败；",
            "修复执行环境（安装 WSL/更换机器/修正路径）后重跑 run_verify；",
        ],
    },
    "build_toolchain_missing": {
        "what": "构建已启动但工具链缺失（退出码 126/127 或 command not found，如 cmake/ccec 不在 PATH）。",
        "fixes": [
            "读 logs.before 指向的日志，确认缺的是哪个工具；",
            "source CANN 环境脚本（set_env.sh）或补装缺失工具后重跑 run_verify；",
        ],
    },
    "catlass_tree_missing": {
        "what": "ASCENDC_CATLASS_ROOT 指向的树缺 scripts/build.sh 或对应 example 目录。",
        "fixes": [
            "修正 ASCENDC_CATLASS_ROOT 指向完整 catlass 树后重试；",
        ],
    },
    "catlass_example_unresolved": {
        "what": (
            "case_dir 缺 manifest.json 的 substrate 字段、且环境未设 "
            "ASCENDC_CATLASS_EXAMPLE，无法确定该覆盖哪个 example。"
        ),
        "fixes": [
            "补 case 的 manifest substrate 字段，或设 ASCENDC_CATLASS_EXAMPLE 环境变量后重试；",
        ],
    },
    "recipe_options_unsupported_for_framework_adapter": {
        "what": "框架适配器未接受本次额外构建参数，构建未启动；参数没有被静默丢弃。",
        "fixes": [
            "完整 CATLASS 根目录可直接作为 case_dir（须有 scripts/build.sh、examples/明确样例）；"
            "也可使用独立样例覆盖源，不需另造仅含部分源码的新 case；",
            "仅当现有 manifest/环境已指定相同 example、arch，且原配方没有其他选项时，"
            "可省略重复参数后重试；soc 使用准确 SKU，例如 Ascend910B1 或 Ascend950，不能传 2201/3510；",
            "若原配方确有其他或不同参数，保留完整 argv，沿原配方在独占副本独立构建并记录日志、"
            "编译与依赖证据；不要删掉参数以迁就适配器，也不要用新 case 逃避历史失败覆盖；",
        ],
    },
    "ambiguous_framework_overlay_source": {
        "what": "待覆盖源内含完整 CATLASS 子树，无法把它视为单一样例源码；构建未启动。",
        "fixes": [
            "核对既有源码与原构建配方，选定唯一待修源；可把既有完整框架根作为 case_dir，"
            "明确 example 与准确 SoC SKU 后验证，不要另复制部分文件创建新 case；",
            "普通目录名不决定源码归属：不得把 framework/vendor 下的文件一律忽略，"
            "也不得把整棵依赖树覆盖进 example；混合源码布局须先消除歧义；",
        ],
    },
}


def _env_preflight_unavailable() -> str | None:
    """verify 前置环境闸门（dry_run 不构建，不走此闸）。

    返回原因码 = 应立即阻断；None = 环境看起来可用，继续。
    设 ``ACD_VERIFY_SKIP_ENV_CHECK=1`` 可显式跳过（留给确知环境可用的用户，
    跳过后若环境真的不行，_classify_build_env_failure 仍会兜住并如实上报）。
    """
    if os.environ.get(_ENV_GATE_SKIP_VAR) == "1":
        return None
    if not detect_cann().get("found"):
        return "cann_not_found"
    if shutil.which("cmake") is None:
        return "cmake_not_found"
    return None


# Track undeclared names and types. Member changes and compiler-suggested
# renames are excluded because legitimate repairs may remove their old spelling.
_ESCAPE_SYMBOL_RES = [
    re.compile(r"(?:use\ of\ )?undeclared\ identifier\ '(?P<sym>[A-Za-z_]\w*)'"),
    re.compile(r"no\ template\ named\ '(?P<sym>[A-Za-z_]\w*)'"),
    re.compile(r"no\ type\ named\ '(?P<sym>[A-Za-z_]\w*)'"),
    re.compile(r"unknown\ type\ name\ '(?P<sym>[A-Za-z_]\w*)'"),
]
_SRC_SUFFIXES_FOR_ESCAPE = {".cpp", ".hpp", ".h", ".cc", ".cce", ".cxx", ".c"}


def _escape_suspect_check(project_dir: Path) -> dict[str, Any] | None:
    """Check that a successful build still retains identifiers tied to the original failure."""
    # Inspect an available original-failure log.
    log_path = None
    for cand in (
        project_dir / "_acd_build_tail.log",
        project_dir / "logs" / "build.log",
        project_dir / "build.log",
    ):
        if cand.is_file():
            log_path = cand
            break
    if log_path is None:
        return None
    try:
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    # Exclude compiler-suggested renames from the identifier-presence heuristic.

    syms: list[str] = []
    for line in log_text.splitlines():
        if "did you mean" in line.lower():
            continue
        for rx in _ESCAPE_SYMBOL_RES:
            m = rx.search(line)
            if m:
                s = m.group("sym")
                if s and s not in syms and len(s) > 2:
                    syms.append(s)
    if not syms:
        return None
    # Indexed SDK symbols may have legitimate migration paths; exclude them here.
    try:
        api_names = {
            str(a.get("name"))
            for a in load_api_signature_kb(PROJECT_ROOT / "harness/knowledge")
        }
    except KnowledgeBaseIntegrityError:
        raise
    except Exception:
        api_names = set()
    local_syms = [s for s in syms if s not in api_names]
    if not local_syms:
        return None
    # Check whether the remaining identifiers occur in the repaired sources.
    src_blobs: list[str] = []
    for p in project_dir.rglob("*"):
        if p.is_file() and p.suffix in _SRC_SUFFIXES_FOR_ESCAPE:
            try:
                src_blobs.append(p.read_text(encoding="utf-8", errors="replace"))
            except OSError:
                pass
    missing = [s for s in local_syms if not any(s in b for b in src_blobs)]
    if not missing:
        return None
    return {
        "check": "original_error_symbols_presence",
        "missing_symbols": missing,
        "evidence_log": str(log_path),
        "advice": (
            f"编译已过，但原始报错点名的符号 {missing} 在修后源码中消失——疑似"
            "删行/绕过式修复（编译过 ≠ 功能保留，这类符号是工程自有定义，正解是"
            "重建定义段而非删除使用点）。若确属应删除的死代码才保留现状；否则"
            "恢复使用点并重建其定义，再验一次。"
        ),
    }


def _environment_unavailable_result(
    reason: str,
    before: dict[str, Any] | None,
    workspace: Path | None,
    dry_run: bool,
    build_invoked: bool,
) -> dict[str, Any]:
    """Return the common result for verification that could not run."""
    guidance = _ENV_REASON_GUIDANCE.get(
        reason, {"what": f"环境级失败：{reason}", "fixes": []}
    )
    skipped = {
        "status": "skipped",
        "signature": None,
        "log_path": None,
        "returncode": None,
    }
    log_tail = None
    if before and before.get("log_path"):
        try:
            text = Path(before["log_path"]).read_text(
                encoding="utf-8", errors="replace"
            )
            log_tail = text[-400:].strip() or None
        except OSError:
            log_tail = None
    next_actions = [
        "本次环境/构建配方条件未满足，尚无有效源码修复结论；先查实际失败条件，保留当前改动。",
        f"发生了什么：{guidance['what']}",
        "按调用者已明确的目标和既有授权修正调用、路径或环境后重试。只有缺少会影响目标、接口、"
        "功能或权限的必要选择时才询问调用者；无新证据不重复相同调用，不绕过验证判据。",
        *guidance["fixes"],
        "可先运行 python scripts/preflight.py 生成环境自检报告一并附给用户。",
        "diagnose（纯日志诊断）不需要编译环境，可继续用它分析已有的 build 日志。",
    ]
    return {
        "before": before or dict(skipped),
        "after": dict(skipped),
        "transition": "environment_unavailable",
        "patch_apply": {
            "status": "skipped",
            "dry_run_ok": None,
            "log_path": None,
            "reason": f"environment gate: {reason}",
        },
        "logs": {"before": (before or {}).get("log_path"), "after": None},
        "workspace": str(workspace) if workspace else None,
        "build_invoked": build_invoked,
        "dry_run": dry_run,
        "verify_infra_error": True,
        "reason": reason,
        "env_detail": guidance["what"],
        "log_tail": log_tail,
        "next_actions": next_actions,
    }


def _verify_impl(
    case_dir: Path,
    patch_file: Path | None = None,
    patch_suggestions: Path | None = None,
    soc: str = "Ascend910B",
    sandbox: bool = False,
    dry_run: bool = False,
    build_args: list[str] | None = None,
    build_target: str | None = None,
    cmake_args: list[str] | None = None,
) -> dict[str, Any]:
    """Run before/after compilation verification in patch or in-place mode."""

    def _build_once(*args, **kwargs):
        options = {
            "build_args": build_args,
            "build_target": build_target,
            "cmake_args": cmake_args,
        }
        kwargs.update({k: v for k, v in options.items() if v})
        label = args[1]
        mode = (
            "env_override"
            if env_override is not None
            else "patch"
            if materialized
            else "in_place"
        )
        recovery.update(
            edge={
                "label": label,
                "mode": mode,
                "workspace": str(args[0]),
                "soc_used": soc_used,
                "soc_source": soc_source,
                "soc_declared": proj_soc,
                "soc_requested": soc,
                "soc_configuration_conflict": bool(
                    proj_soc and soc and proj_soc != soc
                ),
                "before": prior_build[0],
                "patch_apply": applied_patch[0],
            },
            active_stage=None,
        )
        value = _run_build(*args, **kwargs)
        recovery.update(completed_build=value, completed_label=label)
        prior_build[0] = value
        return value

    materialized = None
    prior_build, applied_patch = [None], [None]
    if not dry_run:
        _gate = _env_preflight_unavailable()
        if _gate:
            result = _environment_unavailable_result(
                _gate, None, None, dry_run, build_invoked=False
            )
            return result

    # Use an explicit project target declaration when present.
    soc_used, soc_source = soc, "caller"
    proj_soc = read_project_soc_version(case_dir)
    # Public verify resolves an absent target before calling this implementation.
    # Never silently replace an explicit request with a different project literal.
    if proj_soc and not soc:
        soc_used, soc_source = proj_soc, "project_declaration"

    def _annotate_soc(res: dict[str, Any]) -> dict[str, Any]:
        """Add the selected SoC and source to the top-level verification result."""
        res["soc_requested"] = soc
        res["soc_used"] = soc_used
        res["soc_source"] = soc_source
        res["soc_declared"] = proj_soc
        res["soc_configuration_conflict"] = bool(proj_soc and soc and proj_soc != soc)
        if soc_used != soc:
            res["soc_mismatch_note"] = (
                f"工程 CMakeLists 声明 SOC_VERSION={proj_soc}，与调用值 {soc} 不同；"
                f"已按工程声明 {proj_soc} 编译。若确要按 {soc} 编，请先核对工程意图。"
            )
        return res

    try:
        workspace_case = _copy_case_to_workspace(case_dir)
    except PatchValidationError as exc:
        result = {
            "before": {
                "status": "skipped",
                "signature": None,
                "log_path": None,
                "returncode": None,
            },
            "after": {
                "status": "skipped",
                "signature": None,
                "log_path": None,
                "returncode": None,
            },
            "transition": "case_unsafe",
            "patch_apply": {
                "status": "skipped",
                "dry_run_ok": False,
                "validation_error": str(exc),
            },
            "logs": {"before": None, "after": None},
            "workspace": None,
            "build_invoked": False,
            "dry_run": dry_run,
        }
        return result

    # Apply explicit build-environment overrides from the patch header.
    env_override: dict[str, str] | None = None
    if patch_file and patch_file.exists():
        try:
            env_override = _parse_env_override(read_text(patch_file))
        except PatchValidationError as exc:
            result = {
                "before": {
                    "status": "skipped",
                    "signature": None,
                    "log_path": None,
                    "returncode": None,
                },
                "after": {
                    "status": "skipped",
                    "signature": None,
                    "log_path": None,
                    "returncode": None,
                },
                "transition": "patch_apply_failed",
                "patch_apply": {
                    "status": "failed",
                    "dry_run_ok": False,
                    "validation_error": str(exc),
                    "mode": "env_override",
                },
                "logs": {"before": None, "after": None},
                "workspace": str(workspace_case),
                "build_invoked": False,
                "dry_run": dry_run,
            }
            return result

    if env_override is not None:
        if dry_run:
            result = {
                "before": {
                    "status": "skipped",
                    "signature": None,
                    "log_path": None,
                    "returncode": None,
                },
                "after": {
                    "status": "skipped",
                    "signature": None,
                    "log_path": None,
                    "returncode": None,
                },
                "transition": "dry_run_env_override_ok",
                "patch_apply": {
                    "status": "passed",
                    "dry_run_ok": True,
                    "mode": "env_override",
                    "env_override": env_override,
                },
                "logs": {"before": None, "after": None},
                "workspace": str(workspace_case),
                "build_invoked": False,
                "dry_run": True,
            }
            return result
        before = _build_once(workspace_case, "before", soc=soc_used, sandbox=sandbox)

        _infra = before.get("verify_infra_error")
        if _infra and _infra != "catlass_build_timeout":
            result = _environment_unavailable_result(
                _infra, before, workspace_case, dry_run, build_invoked=True
            )
            return result
        # Temporarily hide the copied build.sh to select the CMake fallback,
        # passing explicit environment overrides to the subprocess.
        applied_patch[0] = {
            "status": "passed",
            "dry_run_ok": True,
            "mode": "env_override",
            "environment_sha256": digest(env_override),
        }
        after = _build_once(
            workspace_case,
            "after",
            soc=soc_used,
            sandbox=sandbox,
            extra_env=env_override,
        )
        result = _annotate_soc(
            {
                "before": before,
                "after": after,
                "transition": _compute_transition(before, after),
                "patch_apply": {
                    "status": "passed",
                    "dry_run_ok": True,
                    "mode": "env_override",
                    "env_override": env_override,
                },
                "logs": {
                    "before": before.get("log_path"),
                    "after": after.get("log_path"),
                },
                "workspace": str(workspace_case),
                "build_invoked": True,
                "dry_run": False,
            }
        )
        return result

    try:
        materialized = _materialize_patch(
            patch_file, patch_suggestions, workspace_case, case_dir
        )
    except PatchValidationError as exc:
        result = {
            "before": {
                "status": "skipped",
                "signature": None,
                "log_path": None,
                "returncode": None,
            },
            "after": {
                "status": "skipped",
                "signature": None,
                "log_path": None,
                "returncode": None,
            },
            "transition": "patch_apply_failed",
            "patch_apply": {
                "status": "failed",
                "dry_run_ok": False,
                "validation_error": str(exc),
            },
            "logs": {"before": None, "after": None},
            "workspace": str(workspace_case),
            "build_invoked": False,
            "dry_run": dry_run,
        }
        return result

    if materialized:
        missing = _check_patch_source_files_exist(workspace_case, materialized)
        if missing:
            result = {
                "before": {
                    "status": "skipped",
                    "signature": None,
                    "log_path": None,
                    "returncode": None,
                },
                "after": {
                    "status": "skipped",
                    "signature": None,
                    "log_path": None,
                    "returncode": None,
                },
                "transition": "no_source_found",
                "patch_apply": {
                    "status": "skipped",
                    "dry_run_ok": False,
                    "missing_source_files": missing,
                },
                "logs": {"before": None, "after": None},
                "workspace": str(workspace_case),
                "build_invoked": False,
                "dry_run": dry_run,
            }
            return result

    if dry_run:
        if not materialized:
            result = {
                "before": {
                    "status": "skipped",
                    "signature": None,
                    "log_path": None,
                    "returncode": None,
                },
                "after": {
                    "status": "skipped",
                    "signature": None,
                    "log_path": None,
                    "returncode": None,
                },
                "transition": "verification_not_run",
                "patch_apply": {
                    "status": "skipped",
                    "dry_run_ok": None,
                    "log_path": None,
                },
                "logs": {"before": None, "after": None},
                "workspace": str(workspace_case),
                "build_invoked": False,
                "dry_run": True,
            }
        else:
            patch_apply = _apply_patch(workspace_case, materialized, dry_run_only=True)
            transition = (
                "dry_run_apply_ok"
                if patch_apply["status"] == "passed"
                else "patch_apply_failed"
            )
            result = {
                "before": {
                    "status": "skipped",
                    "signature": None,
                    "log_path": None,
                    "returncode": None,
                },
                "after": {
                    "status": "skipped",
                    "signature": None,
                    "log_path": None,
                    "returncode": None,
                },
                "transition": transition,
                "patch_apply": patch_apply,
                "logs": {"before": None, "after": None},
                "workspace": str(workspace_case),
                "build_invoked": False,
                "dry_run": True,
            }
        return result

    before = _build_once(workspace_case, "before", soc=soc_used, sandbox=sandbox)
    build_invoked = True

    # Recipe setup failures are infrastructure errors; a compiler timeout is a failed build attempt.
    _infra = before.get("verify_infra_error")
    if _infra and _infra != "catlass_build_timeout":
        result = _environment_unavailable_result(
            _infra, before, workspace_case, dry_run, build_invoked=True
        )
        return result
    after: dict[str, Any] = {
        "status": "skipped",
        "signature": None,
        "log_path": None,
        "returncode": None,
    }
    transition = "verification_not_run"
    patch_apply: dict[str, Any] = {
        "status": "skipped",
        "dry_run_ok": None,
        "log_path": None,
    }
    verify_mode = "patch"
    not_reproducible = False

    if materialized:
        patch_apply = _apply_patch(workspace_case, materialized)
        if patch_apply["status"] == "passed":
            applied_patch[0] = patch_apply
            after = _build_once(workspace_case, "after", soc=soc_used, sandbox=sandbox)
            transition = _compute_transition(before, after)

            # Patch and in-place modes use the same compilation-coverage checks.
            if transition == "compile_failure_fixed":
                if (
                    _sources_seen_in_build(workspace_case, after.get("log_path"))
                    is False
                ):
                    transition = "case_not_reproducible"
                    not_reproducible = True
        else:
            after = {
                "status": "patch_apply_failed",
                "signature": None,
                "log_path": patch_apply.get("log_path"),
                "returncode": patch_apply.get("returncode"),
            }
            transition = "patch_apply_failed"
    elif before.get("status") == "failed":
        # Without a patch file, build the supplied source state directly.
        # This mode verifies the current checkout and does not invent a prior failure.
        transition = "same_error_remaining"
        verify_mode = "in_place"
        after = dict(before)  # In-place mode uses this build as the after-state.
        patch_apply = {
            "status": "not_applicable",
            "dry_run_ok": None,
            "log_path": None,
            "reason": "in_place_mode: no patch supplied, verified case_dir as-is",
        }
    elif before.get("status") == "passed":
        # A successful in-place build still requires source-coverage evidence.
        verify_mode = "in_place"
        after = dict(before)
        patch_apply = {
            "status": "not_applicable",
            "dry_run_ok": None,
            "log_path": None,
            "reason": "in_place_mode: no patch supplied, verified case_dir as-is",
        }

        # Do not confirm a repair when the successful build did not cover device sources.
        seen = _sources_seen_in_build(workspace_case, before.get("log_path"))
        if seen is False:
            transition = "case_not_reproducible"
            not_reproducible = True
        else:
            transition = "compile_failure_fixed"

    result = _annotate_soc(
        {
            "before": before,
            "after": after,
            "transition": transition,
            "patch_apply": patch_apply,
            "verify_mode": verify_mode,
            "logs": {"before": before.get("log_path"), "after": after.get("log_path")},
            "workspace": str(workspace_case),
            "build_invoked": build_invoked,
            "dry_run": False,
        }
    )
    # Expose the actual framework target so consumers can verify build identity.
    if before.get("built_target") is not None:
        result["built_target"] = before.get("built_target")
        result["expected_target"] = before.get("expected_target")
        result["example_source"] = before.get("example_source")

        # Expose the actual architecture together with the built target.
        result["built_arch"] = before.get("built_arch")
        result["arch_source"] = before.get("arch_source")
    if not_reproducible:
        # Missing source-coverage evidence cannot confirm a repair.
        result["verify_infra_error"] = True
        result["reason"] = "build_passed_without_compiling_kernel_sources"
        result["next_actions"] = [
            "本次构建返回成功,但日志里【一次都没出现 kernel 源文件名】——说明这套构建流程"
            "只编了 host 侧,**没有编译 device(kernel) 代码**。常见于 msopgen 风格算子工程:"
            "默认 target 只把 kernel 源码打包 + 生成算子信息(copy_kernel_srcs / ops_info_gen),"
            "真正的 kernel 编译要显式 `cmake --build build --target binary`。",
            "【不要】把这个结果当作修复已验证——kernel 里的错误在这次构建中根本不可见。"
            "请用能真编 device 代码的方式复核:`cmake --build build --target binary`,"
            "或直接调 ccec/bisheng 编译该 kernel 文件。",
            "The selected build recipe did not compile the kernel; verify the case recipe before judging a fix.",
            "Complete operators may require both host and device build stages; a host-only build cannot prove a kernel fix.",
        ]

    # Identifier disappearance is advisory and requires a project-contract review.
    if transition == "compile_failure_fixed":
        suspect = _escape_suspect_check(case_dir)
        if suspect:
            result["escape_suspect"] = suspect

    # Diagnose the current failure so guidance does not remain tied to an earlier error.

    if (
        transition in ("same_error_remaining", "new_compile_error")
        and after.get("status") == "failed"
    ):
        result["next_actions"] = [
            "若 log_tail 里的主报错与最初诊断时不是同一条（位置/形态变了）：先把新 tail 存成文件，"
            "再调 diagnose_compile_failure(case_dir, 新日志路径) 重新诊断一次——"
            "did-you-mean/快速路径建议只对当前报错有效，旧诊断不覆盖新错误。",
        ]

    # Environment failures use the shared result constructor.

    # Compilation facts remain separate from the applicability of versioned knowledge.
    try:
        _vc_env = (detect_cann() or {}).get("version")
    except Exception:
        _vc_env = None
    result["version_compatibility"] = classify_version(None, _vc_env)
    if is_mismatch(result["version_compatibility"]):
        result["version_mismatch_note"] = (
            "验证机 CANN 低于本工具支持下限——本次编译验证的事实（编过/编不过）"
            "如实有效，但版本相关结论仅供参考；以目标版本 toolkit 的登记域为准，"
            "必要时在受支持版本的验证机上复验。"
        )
    return result


def _previous_failure_evidence(value: dict) -> dict:
    """Retain target facts from an older full verify result when its evidence omitted them."""
    if "failure_evidence" not in value:
        return value
    evidence = value["failure_evidence"]
    if not isinstance(evidence, dict):
        raise ValueError("prior failure_evidence is not an object")
    evidence = dict(evidence)
    after = value.get("after") or {}
    before = value.get("before") or {}
    if not isinstance(after, dict) or not isinstance(before, dict):
        raise ValueError("prior verify stages are not objects")
    stage = after if after.get("status") not in {None, "skipped"} else before
    for field, source_field in (
        ("build_target", "expected_target"),
        ("selected_example", "selected_example"),
    ):
        candidates = {
            item
            for item in (
                evidence.get(field),
                stage.get(source_field),
                value.get(source_field),
            )
            if isinstance(item, str) and item.strip()
        }
        if len(candidates) > 1:
            raise ValueError(
                f"prior verify result has conflicting {source_field} evidence"
            )
        if candidates:
            evidence[field] = candidates.pop()
    return evidence


def verify(
    case_dir: Path,
    patch_file: Path | None = None,
    patch_suggestions: Path | None = None,
    soc: str | None = None,
    sandbox: bool = False,
    dry_run: bool = False,
    previous_result: Path | None = None,
    build_args: list[str] | None = None,
    build_target: str | None = None,
    cmake_args: list[str] | None = None,
    recovery_dir: Path | None = None,
) -> dict[str, Any]:
    case_dir = Path(case_dir)
    patch_file = Path(patch_file) if patch_file else None
    patch_suggestions = Path(patch_suggestions) if patch_suggestions else None
    previous_result = Path(previous_result) if previous_result else None
    recovery_dir = Path(recovery_dir) if recovery_dir else None
    options = dict(
        patch_file=patch_file,
        patch_suggestions=patch_suggestions,
        soc=soc,
        sandbox=sandbox,
        dry_run=dry_run,
        previous_result=previous_result,
        build_args=build_args,
        build_target=build_target,
        cmake_args=cmake_args,
    )
    if dry_run:
        return _verify_request(case_dir, **options)
    directory = (
        recovery_dir or (_workspace_root() / "verification" / uuid.uuid4().hex)
    ).resolve()
    request = {
        "case_dir": str(case_dir.resolve()),
        **{k: str(v) if isinstance(v, Path) else v for k, v in options.items()},
        "files": {
            str(p.resolve()): recovery.file_hash(p)
            for p in (patch_file, patch_suggestions, previous_result)
            if p
        },
    }
    if previous_result:
        try:
            request["previous_snapshot"] = load_json(previous_result)
        except (OSError, ValueError):
            pass  # Existing finalizer reports invalid prior evidence.
    with recovery.session(directory, request):
        result = _verify_request(case_dir, **options)
        result["recovery"] = {"directory": str(directory), "status": "recorded"}
        recovery.save(directory / "result.json", result)
        return result


def _verify_request(
    case_dir: Path,
    patch_file: Path | None = None,
    patch_suggestions: Path | None = None,
    soc: str | None = None,
    sandbox: bool = False,
    dry_run: bool = False,
    previous_result: Path | None = None,
    build_args: list[str] | None = None,
    build_target: str | None = None,
    cmake_args: list[str] | None = None,
) -> dict[str, Any]:
    """Verify in an owned workspace; compare explicit prior evidence when supplied.

    A successful process is confirmed only by compiler records, objects and
    dependency coverage. A new in-place failure has no implied predecessor.
    """
    case_dir = case_dir.resolve()
    if soc in {"2201", "3510"}:
        stopped = {
            "status": "not_started",
            "returncode": None,
            "build_invoked": False,
            "log_path": None,
        }
        result = {
            "transition": "verification_not_run",
            "before": stopped,
            "after": dict(stopped),
            "build_invoked": False,
            "dry_run": dry_run,
            "reason": "soc_requires_sku",
            "workspace": None,
            "logs": {"before": None, "after": None},
            "soc_requested": soc,
            "validation_scope": {
                "compile": "not_run",
                "contract": "not_checked",
                "numerical": "not_checked",
            },
            "error_comparison": {
                "status": "unavailable",
                "reason": "build_not_started",
            },
            "next_actions": [
                "soc requires a SoC SKU, for example Ascend910B1 or Ascend950. "
                "2201/3510 are architecture identifiers, not SKU values.",
                "Confirm the intended SKU from the original recipe, then retry. "
                "No build ran; the requested target was not automatically rewritten.",
            ],
        }
        result["audit"] = _emit_verify_audit(
            "verify",
            {
                "case_dir": str(case_dir),
                "transition": result["transition"],
                "build_invoked": False,
            },
        )
        return result
    # CMake cache assignments require -D. Preserve raw argv rather than silently
    # correcting it, and do not manufacture a new source error from CLI misuse.
    if any(
        re.match(r"^[A-Za-z_][A-Za-z0-9_]*(?::[A-Za-z_]+)?=", arg)
        for arg in cmake_args or []
    ):
        stopped = {
            "status": "not_started",
            "returncode": None,
            "build_invoked": False,
            "log_path": None,
        }
        result = {
            "transition": "verification_not_run",
            "before": stopped,
            "after": dict(stopped),
            "build_invoked": False,
            "dry_run": dry_run,
            "reason": "cmake_cache_argument_missing_define",
            "workspace": None,
            "logs": {"before": None, "after": None},
            "soc_requested": soc,
            "validation_scope": {
                "compile": "not_run",
                "contract": "not_checked",
                "numerical": "not_checked",
            },
            "error_comparison": {
                "status": "unavailable",
                "reason": "build_not_started",
            },
            "next_actions": [
                "CMake cache assignments need the -D prefix. Pass each complete argv token, "
                "for example --cmake-arg=-DOPTION=VALUE.",
                "Correct the invocation using the original recipe, then verify again. "
                "No build ran and this result says nothing about the source fix. "
                "Arguments are not automatically rewritten.",
            ],
        }
        result["audit"] = _emit_verify_audit(
            "verify",
            {
                "case_dir": str(case_dir),
                "transition": result["transition"],
                "build_invoked": False,
            },
        )
        return result
    manifest_arch = str(_manifest_json(case_dir).get("arch") or "")
    inferred_soc = "Ascend950" if manifest_arch == "3510" else None
    project_soc = read_project_soc_version(case_dir)
    framework_soc = None
    if not soc and not manifest_arch and not project_soc:
        example, _ = _resolve_catlass_example(case_dir)
        if (
            _full_catlass_case(case_dir, example)
            and os.environ.get("ASCENDC_CATLASS_ARCH") == "3510"
        ):
            framework_soc = "Ascend950"
    used_soc = soc or inferred_soc or project_soc or framework_soc
    recovery.update(
        resolved={
            "soc": soc,
            "used_soc": used_soc,
            "inferred_soc": inferred_soc,
            "project_soc": project_soc,
            "framework_soc": framework_soc,
            "dry_run": dry_run,
            "previous_result": str(previous_result) if previous_result else None,
            "build_args": build_args,
            "build_target": build_target,
            "cmake_args": cmake_args,
        }
    )
    result = _verify_impl(
        case_dir,
        patch_file,
        patch_suggestions,
        soc=used_soc,
        sandbox=sandbox,
        dry_run=dry_run,
        build_args=build_args,
        build_target=build_target,
        cmake_args=cmake_args,
    )
    recovery.update(raw_result=result)
    return _finalize_verify_result(
        result,
        case_dir,
        soc=soc,
        used_soc=used_soc,
        inferred_soc=inferred_soc,
        project_soc=project_soc,
        framework_soc=framework_soc,
        dry_run=dry_run,
        previous_result=previous_result,
        build_args=build_args,
        build_target=build_target,
        cmake_args=cmake_args,
        previous_snapshot=(recovery.CURRENT.get() or {})
        .get("state", {})
        .get("request", {})
        .get("previous_snapshot"),
    )


def _finalize_verify_result(
    result: dict,
    case_dir: Path,
    *,
    soc=None,
    used_soc=None,
    inferred_soc=None,
    project_soc=None,
    framework_soc=None,
    dry_run=False,
    previous_result=None,
    build_args=None,
    build_target=None,
    cmake_args=None,
    previous_snapshot=None,
) -> dict:
    """Interpret existing build facts; this function never starts a build."""
    result["soc_requested"] = soc
    if result.get("soc_source") != "project_declaration":
        # Preserve the project-declaration provenance selected by this module.
        result["soc_source"] = (
            "caller"
            if soc
            else "manifest"
            if inferred_soc
            else "framework_env"
            if framework_soc
            else "project_declaration"
            if project_soc
            else "unknown"
        )
    # Preflight/dry-run/invalid patch exits contain no compile evidence.
    if not result.get("build_invoked") or dry_run:
        if (
            not used_soc
            and not dry_run
            and result.get("verify_infra_error") == "requested_soc_unknown"
        ):
            result.update(
                transition="verification_not_run",
                reason="requested_soc_unknown",
                validation_scope={
                    "compile": "not_run",
                    "contract": "not_checked",
                    "numerical": "not_checked",
                },
            )
        result["audit"] = _emit_verify_audit(
            "verify_dry_run" if dry_run else "verify",
            {
                "case_dir": str(case_dir),
                "transition": result["transition"],
                "build_invoked": result.get("build_invoked", False),
            },
        )
        return result
    before, after = result.get("before") or {}, result.get("after") or {}
    current = after if after.get("status") not in {None, "skipped"} else before
    invoked = [
        b.get("build_invoked", b.get("returncode") is not None)
        for b in (before, after)
        if b
    ]
    result["build_invoked"] = (
        True
        if any(x is True for x in invoked)
        else None
        if any(x is None for x in invoked)
        else False
    )
    if any(b.get("status") in {"unknown", "running"} for b in (before, after)):
        result.update(
            transition="verification_incomplete",
            reason="execution_unconfirmed",
            validation_scope={
                "compile": "unconfirmed",
                "contract": "not_checked",
                "numerical": "not_checked",
            },
        )
        return result
    recipe_request = {
        "soc_used": used_soc,
        "build_args": list(build_args or []),
        "build_target": build_target,
        "cmake_args": list(cmake_args or []),
    }
    for build in (before, after):
        ev = build.get("failure_evidence")
        if ev:
            ev["project_id"] = digest(str(case_dir))
            ev["target"] = result.get("soc_used") or used_soc
            ev["recipe_request"] = recipe_request
            ev["recipe_request_sha256"] = digest(recipe_request)
            if (
                isinstance(build.get("expected_target"), str)
                and build["expected_target"]
            ):
                ev["build_target"] = build["expected_target"]
            if (
                isinstance(build.get("selected_example"), str)
                and build["selected_example"]
            ):
                ev["selected_example"] = build["selected_example"]
            ev["evidence_id"] = digest(
                {k: v for k, v in ev.items() if k != "evidence_id"}
            )
    evidence = current.get("failure_evidence")
    result["failure_evidence"] = evidence
    previous = None
    if previous_result:
        try:
            previous = (
                previous_snapshot
                if previous_snapshot is not None
                else json.loads(Path(previous_result).read_text(encoding="utf-8"))
            )
            if not isinstance(previous, dict):
                raise ValueError("prior evidence is not an object")
            previous = _previous_failure_evidence(previous)
        except (OSError, ValueError, TypeError) as exc:
            result["previous_evidence_error"] = str(exc)
            previous = None
    elif result.get("verify_mode") != "in_place":
        previous = before.get("failure_evidence")
    comparison = (
        compare_issues(previous, evidence)
        if evidence
        else {"status": "unavailable", "reason": "current_diagnostics_missing"}
    )
    result["error_comparison"] = comparison
    if current.get("status") == "interrupted":
        result["transition"] = "verification_interrupted"
        result["reason"] = current.get("interruption_reason")
        result.pop("verify_infra_error", None)
    elif current.get("status") == "passed":
        actual = current.get("compilation_evidence") or {"status": "unconfirmed"}
        result["compilation_evidence"] = actual
        arches = actual.get("architectures", [])
        targets = actual.get("targets", [])
        result["built_arch"] = arches[0] if len(arches) == 1 else (arches or None)
        result["built_target"] = targets[0] if len(targets) == 1 else (targets or None)
        result["expected_target"] = current.get("expected_target")
        expected_arch = ("3510" if "950" in used_soc else "2201") if used_soc else None
        required = actual.get("required_sources") or []
        expected_target = current.get("expected_target")
        source_scope = actual.get("source_scope") or {}
        configured_targets = (
            set(source_scope.get("target_names") or [])
            if source_scope.get("status") == "configured"
            and source_scope.get("requested_target") == expected_target
            else set()
        )
        matching_records = [
            r
            for r in actual.get("records", [])
            if r.get("architectures") == [expected_arch]
            and (
                not expected_target
                or r.get("target") == expected_target
                or r.get("target") in configured_targets
            )
        ]
        covered = {p for r in matching_records for p in r.get("dependencies", [])}
        target_matched = (
            not expected_target
            or bool(configured_targets)
            or any(r.get("target") == expected_target for r in matching_records)
        )
        matched = (
            actual.get("status") == "confirmed"
            and bool(matching_records)
            and set(required).issubset(covered)
            and target_matched
        )
        failure_coverage = prior_failure_coverage(
            previous, {**actual, "records": matching_records}
        )
        result["target_validation"] = {
            "requested_arch": expected_arch,
            "observed_architectures": arches,
            "prior_failure_coverage": failure_coverage,
            "status": "matched" if matched else "unconfirmed",
        }
        repaired = (
            comparison.get("status") == "compared"
            and comparison.get("build_target_comparison", {}).get("status") == "matched"
            and (previous or {}).get("status") == "failed"
            and failure_coverage["status"] != "unconfirmed"
        )
        result["transition"] = (
            ("compile_failure_fixed" if repaired else "compile_passed")
            if matched
            else "case_not_reproducible"
        )
        if not matched:
            result["verify_infra_error"] = True
            result["reason"] = (
                "compilation_of_requested_sources_and_architecture_unconfirmed"
            )
            result["next_actions"] = [
                "Inspect compilation_evidence and the full build log. Preserve the original recipe; "
                "confirm the intended device target, source dependency coverage and architecture."
            ]
        else:
            result.pop("verify_infra_error", None)
            result.pop("reason", None)
            result.pop("next_actions", None)
        # A successful build alone does not prove that required behavior was retained.
        contract = {"status": "not_applicable"}
        # Compare retained behavior only against a prior failure of the same project
        # and target. Unrelated historical results cannot establish a removal claim.
        if matched and comparison.get("status") == "compared":
            prior_issues = (previous or {}).get("issues") or []
            source_root = current.get("compile_source_root") or str(Path(case_dir))
            contract = identifier_retention(prior_issues, source_root)
        result["contract_check"] = contract
        if contract["status"] == "violated":
            vanished = ", ".join(m["identifier"] for m in contract["missing"])
            result["transition"] = "compile_passed"
            result["reason"] = "functionality_identifiers_removed"
            result["next_actions"] = [
                f"Compilation passed, but failed-on identifiers ({vanished}) no longer occur in the "
                "repaired sources. Two possible shapes exist and the token absence alone cannot "
                "distinguish them — decide which one this repair is: (a) a deliberate global rename of "
                "a multiply-misspelled/hallucinated name — then the new tokens are the repaired calls; "
                "record the old->new mapping in the handoff material; (b) deleted call sites — then "
                "restore the removed uses and repair them properly (define the missing type/contract "
                "or correct the call); deleting functionality is not a valid repair. Check whether the "
                "old name was ever declared in the pre-repair project (backup/git/原错误文件上下文); "
                "names that only ever existed in SDK headers or as hallucinations belong to shape (a). "
                "Re-run verify with --previous-result if the repair changes."
            ]
        result["validation_scope"] = {
            "compile": "confirmed" if matched else "unconfirmed",
            "contract": contract["status"]
            if contract["status"] in ("violated", "ok")
            else "not_checked",
            "numerical": "not_checked",
        }
    elif current.get("status") == "failed" and not current.get("verify_infra_error"):
        if comparison["status"] != "compared":
            transition = "compile_failure_remaining"
        elif comparison["observed_new"]:
            transition = "new_compile_error"
        else:
            transition = "same_error_remaining"
        result["transition"] = transition
        result["next_actions"] = [
            "Use the complete current log at logs.after (or logs.before). Compare error_comparison; "
            "diagnose again only when the new evidence changes the current repair hypothesis. "
            "Pass this verify.json with --previous-result on the next verification."
        ]
    result["audit"] = _emit_verify_audit(
        "verify",
        {
            "case_dir": str(case_dir),
            "transition": result["transition"],
            "build_invoked": result["build_invoked"],
        },
    )
    return result


# Device-source coverage must be checked separately from successful host compilation.
_BUILD_EVIDENCE_SUFFIXES = (".cpp", ".cc", ".cxx", ".asc", ".c")
# Recognize conventional device-source locations and kernel filename suffixes.
_KERNEL_DIR_MARKERS = ("op_kernel",)
_KERNEL_NAME_MARKERS = ("_kernel",)


def _default_cmake_targets(build: Path, targets: dict[str, dict]) -> set[str] | None:
    """Read the generated default dependency graph without executing its recipes.

    The codemodel includes EXCLUDE_FROM_ALL targets. Only the generator's
    default graph can establish which of those targets the default build asks
    for. Unsupported or ambiguous graph syntax keeps the conservative fallback.
    """
    graph: dict[str, set[str]] = {}
    makefile = build / "CMakeFiles" / "Makefile2"
    ninja = build / "build.ninja"
    if makefile.is_file():
        for line in read_text(makefile).replace("\\\n", " ").splitlines():
            if line.startswith(("#", "\t")) or "=" in line:
                continue
            left, sep, right = line.partition(":")
            if not sep or "$" in left + right:
                continue
            try:
                names, dependencies = shlex.split(left), shlex.split(right)
            except ValueError:
                return None
            for name in names:
                graph.setdefault(name, set()).update(dependencies)
        default = "all"
    elif ninja.is_file():

        def words(value: str) -> list[str] | None:
            tokens = re.findall(r"(?:\$[ $:]|[^\s])+", value)
            if any(re.search(r"\$(?![ $:])", token) for token in tokens):
                return None
            return [re.sub(r"\$([ $:])", r"\1", token) for token in tokens]

        default = "all"
        for line in read_text(ninja).replace("$\n", "").splitlines():
            if line.startswith("default "):
                values = words(line[8:])
                if not values or len(values) != 1:
                    return None
                default = values[0]
            elif line.startswith("build "):
                parts = re.split(r"(?<!\$):\s+", line[6:], maxsplit=1)
                if len(parts) != 2:
                    continue
                names, values = words(parts[0]), words(parts[1])
                if names is None or values is None:
                    return None
                for name in names:
                    graph.setdefault(name, set()).update(
                        item for item in values[1:] if item not in {"|", "||"}
                    )
    else:
        return None
    if default not in graph:
        return None
    pending, visited = [default], set()
    while pending:
        name = pending.pop()
        if name in visited:
            continue
        visited.add(name)
        pending.extend(graph.get(name, ()))
    names = {
        match[1]
        for node in visited
        if (match := re.search(r"(?:^|/)CMakeFiles/([^/]+)\.dir/all$", node))
    }
    return {
        key
        for key, target in targets.items()
        if target.get("name") in names | visited
        or any(a.get("path") in visited for a in target.get("artifacts", []))
    }


def _configured_kernel_sources(
    build_root: Path, project: Path, target: str | None
) -> tuple[list[Path], dict[str, Any]]:
    """Read a configured scope, retaining requirements when metadata is invalid."""
    try:
        return _read_configured_kernel_sources(build_root, project, target)
    except (OSError, ValueError, TypeError, KeyError, AttributeError):
        return _kernel_sources(project), {
            "status": "conservative_fallback",
            "basis": "source_tree",
            "requested_target": target,
            "reason": "configuration_metadata_invalid",
        }


def _read_configured_kernel_sources(
    build_root: Path, project: Path, target: str | None
) -> tuple[list[Path], dict[str, Any]]:
    """Bind requirements to configuration, independently of successful compiles.

    A complete, unambiguous CMake codemodel is an execution plan, not evidence
    that any object was produced. Every selected device translation unit stays
    required even when its compiler invocation or object is absent. Vendor
    custom commands without a declared device compilation group and builds
    without a usable codemodel retain the conservative source-tree requirement.
    """
    project, build_root = project.resolve(), build_root.resolve()
    fallback = _kernel_sources(project)

    def unknown(reason: str) -> tuple[list[Path], dict[str, Any]]:
        return fallback, {
            "status": "conservative_fallback",
            "basis": "source_tree",
            "requested_target": target,
            "reason": reason,
        }

    models = []
    for directory, dirs, files in os.walk(build_root):
        dirs[:] = [
            d for d in dirs if d not in {".git", ".venv", "logs", "node_modules"}
        ]
        reply = Path(directory)
        if reply.parts[-4:] != (".cmake", "api", "v1", "reply"):
            continue
        indexes = sorted(
            name
            for name in files
            if name.startswith("index-") and name.endswith(".json")
        )
        if not indexes:
            continue
        index_path = reply / indexes[-1]
        index = load_json(index_path, default={}) or {}
        entry = index.get("reply", {}).get("codemodel-v2", {})
        filename = entry.get("jsonFile")
        if not isinstance(filename, str) or Path(filename).name != filename:
            continue
        model = load_json(reply / filename, default={}) or {}
        paths = model.get("paths", {})
        source = Path(paths.get("source", "")).resolve()
        build = Path(paths.get("build", "")).resolve()
        # Never consume configuration for a different project or another run.
        if not (source == project or source in project.parents):
            continue
        if build != reply.parents[3] or not build.is_relative_to(build_root):
            continue
        if not source.is_relative_to(build_root):
            continue
        if model.get("version", {}).get("major") != 2:
            continue
        models.append((reply, index_path, model))
    if len(models) != 1:
        return unknown("configuration_missing_or_ambiguous")
    reply, index_path, model = models[0]
    configurations = model.get("configurations", [])
    if len(configurations) != 1:
        return unknown("configuration_selection_unresolved")
    targets: dict[str, dict] = {}
    for entry in configurations[0].get("targets", []):
        filename = entry.get("jsonFile")
        if not isinstance(filename, str) or Path(filename).name != filename:
            return unknown("target_configuration_incomplete")
        data = load_json(reply / filename, default={}) or {}
        if not data.get("id") or data.get("id") != entry.get("id"):
            return unknown("target_configuration_incomplete")
        targets[data["id"]] = data
    if target and target != "all":
        selected = {key for key, data in targets.items() if data.get("name") == target}
        if len(selected) != 1:
            return unknown("requested_target_not_in_configuration")
    else:
        selected = _default_cmake_targets(Path(model["paths"]["build"]), targets)
        if selected is None:
            return unknown("default_target_graph_unresolved")
    pending, closure = list(selected), set()
    while pending:
        key = pending.pop()
        if key in closure:
            continue
        if key not in targets:
            return unknown("target_dependency_configuration_incomplete")
        closure.add(key)
        pending.extend(dep["id"] for dep in targets[key].get("dependencies", []))
    required = set()
    source_root = Path(model["paths"]["source"])
    conventional = {path.resolve() for path in fallback}
    for key in closure:
        data = targets[key]
        groups = data.get("compileGroups", [])
        for source in data.get("sources", []):
            group_index = source.get("compileGroupIndex")
            if not isinstance(group_index, int) or not 0 <= group_index < len(groups):
                continue
            group = groups[group_index]
            name = source.get("path")
            if not isinstance(name, str):
                return unknown("source_configuration_incomplete")
            path = Path(name)
            path = (path if path.is_absolute() else source_root / path).resolve()
            if not path.is_relative_to(project):
                continue
            flags = " ".join(
                str(item.get("fragment", ""))
                for item in group.get("compileCommandFragments", [])
            )
            if (
                group.get("language") == "ASC"
                or "--npu-arch" in flags
                or path in conventional
                or path.suffix.lower() in {".asc", ".cce"}
            ):
                required.add(path)
    if not required:
        return unknown("device_sources_not_declared_in_configuration")
    return sorted(required), {
        "status": "configured",
        "basis": "cmake_file_api",
        "configuration_index": str(index_path),
        "configuration_sha256": hashlib.sha256(index_path.read_bytes()).hexdigest(),
        "requested_target": target,
        "target_names": sorted(targets[key]["name"] for key in closure),
        "excluded_source_tree_candidates": sorted(
            str(path) for path in conventional - required
        ),
        "reason": None,
    }


def _kernel_sources(case_dir: Path) -> list[Path]:
    """收集本工程的 device 侧源码（Ascend C 的 kernel 代码）。"""
    out: list[Path] = []
    for suffix in _BUILD_EVIDENCE_SUFFIXES:
        for src in case_dir.rglob(f"*{suffix}"):
            if any(
                part in _NON_SOURCE_DIRS for part in src.relative_to(case_dir).parts
            ):
                continue
            rel = src.relative_to(case_dir).as_posix().lower()
            is_kernel = (
                any(m in rel for m in _KERNEL_DIR_MARKERS)
                or any(m in src.stem.lower() for m in _KERNEL_NAME_MARKERS)
                or src.suffix.lower() == ".asc"
            )
            if is_kernel:
                out.append(src)
    return out


def _sources_seen_in_build(case_dir: Path, build_log_path: str | None) -> bool | None:
    """Check whether the build log shows compilation of the project's kernel source."""
    if not build_log_path:
        return None
    log_file = Path(build_log_path)
    if not log_file.exists():
        return None
    kernels = _kernel_sources(case_dir)
    if not kernels:
        return None
    try:
        text = log_file.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    if any(src.name in text for src in kernels):
        return True

    # Inspect direct includes from compiled siblings as a bounded coverage fallback.
    # This does not attempt full preprocessing or dependency closure.
    for src in kernels:
        for sibling_dir in (src.parent, case_dir):
            for sibling in sibling_dir.glob("*"):
                if sibling.suffix.lower() not in _BUILD_EVIDENCE_SUFFIXES:
                    continue
                if sibling.name not in text or sibling == src:
                    continue
                try:
                    sibling_text = sibling.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    continue
                for line in sibling_text.splitlines():
                    stripped = line.strip()
                    if stripped.startswith("#include") and src.name in stripped:
                        return True
    return False


def _compute_transition(before: dict[str, Any], after: dict[str, Any]) -> str:
    # An unchanged failure requires two failed builds with matching signatures.
    # A passing before-state followed by failure is a new compile error.
    if after.get("status") == "passed":
        return "compile_failure_fixed"
    if (
        before.get("status") == "failed"
        and before.get("signature")
        and before.get("signature") == after.get("signature")
    ):
        return "same_error_remaining"
    return "new_compile_error"


def main() -> int:
    force_utf8_stdio()
    import sys

    if any(x.split("=", 1)[0] == "--recover-verification" for x in sys.argv[1:]):
        collector = argparse.ArgumentParser(
            description="Collect verification evidence; never launch or patch."
        )
        collector.add_argument("--recover-verification", type=Path, required=True)
        collector.add_argument("--json-out", type=Path, required=True)
        args = collector.parse_args()
        if args.json_out.resolve().is_relative_to(args.recover_verification.resolve()):
            collector.error(
                "--json-out must be outside the verification checkpoint directory"
            )
        if args.json_out.exists():
            collector.error(
                "recovery output must be a new file; preserve existing evidence"
            )
        data = recover_verification(args.recover_verification)
        write_json(args.json_out, data)
        print(str(args.json_out))
        return 0
    if any(x.split("=", 1)[0] == "--observe-run" for x in sys.argv[1:]):
        observer = argparse.ArgumentParser(
            description="Observe a durable execution; never launch a build."
        )
        observer.add_argument("--observe-run", type=Path, required=True)
        observer.add_argument("--expected-identity")
        args = observer.parse_args()
        print(
            json.dumps(
                observe_run(args.observe_run, expected_identity=args.expected_identity),
                ensure_ascii=False,
            )
        )
        return 0
    parser = argparse.ArgumentParser(
        description="Run before/after compile verification for a case."
    )
    parser.add_argument("--case", required=True)
    parser.add_argument("--apply-patch")
    parser.add_argument("--patch-suggestions")
    parser.add_argument("--soc")
    parser.add_argument(
        "--previous-result", help="Previous verify.json or failure_evidence.json"
    )
    parser.add_argument(
        "--build-arg",
        action="append",
        help="Original build.sh argument; repeat, use = for flags",
    )
    parser.add_argument(
        "--target",
        action="append",
        help="One explicit CMake build target (projects without build.sh); do not repeat",
    )
    parser.add_argument(
        "--cmake-arg",
        action="append",
        help="Complete original CMake argv token; repeat, e.g. --cmake-arg=-DOPTION=VALUE",
    )
    parser.add_argument("--json-out", required=True)
    parser.add_argument(
        "--recovery-dir",
        type=Path,
        help="New owned checkpoint directory; preserve it with the build workspace.",
    )
    parser.add_argument(
        "--sandbox",
        action="store_true",
        help="Wrap the build in firejail/unshare -rn when available (no-op otherwise).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Only validate the patch and dry-run; never invoke the build.",
    )
    args = parser.parse_args()
    if args.recovery_dir and Path(args.json_out).resolve().is_relative_to(
        args.recovery_dir.resolve()
    ):
        parser.error("--json-out must be outside the verification checkpoint directory")
    if args.target and len(args.target) != 1:
        parser.error(
            "--target may be specified only once. Preserve a multi-target recipe "
            "through the original build.sh arguments, or verify each target separately "
            "and keep every report."
        )
    if args.target and (not args.target[0].strip() or args.target[0].startswith("-")):
        parser.error("--target requires one nonempty target name, not an option")
    recovery_dir = args.recovery_dir or (
        _workspace_root() / "verification" / uuid.uuid4().hex
    )
    failed = False
    try:
        data = verify(
            Path(args.case).resolve(),
            Path(args.apply_patch).resolve() if args.apply_patch else None,
            Path(args.patch_suggestions).resolve() if args.patch_suggestions else None,
            soc=args.soc,
            sandbox=args.sandbox,
            dry_run=args.dry_run,
            previous_result=Path(args.previous_result).resolve()
            if args.previous_result
            else None,
            build_args=args.build_arg,
            build_target=args.target[0] if args.target else None,
            cmake_args=args.cmake_arg,
            recovery_dir=recovery_dir,
        )
    except Exception as exc:
        failed = True
        failure = getattr(
            exc,
            "verification_failure",
            {
                "directory": str(recovery_dir.resolve()),
                "build_invoked": False,
                "executions": [],
                "new_build_invoked": False,
            },
        )
        data = {
            "before": {},
            "after": {},
            "transition": (
                "verification_not_run"
                if failure["build_invoked"] is False
                else "verification_incomplete"
            ),
            "logs": {},
            "build_invoked": failure["build_invoked"],
            "recovery": {**failure, "status": "needs_observation"},
            "validation_scope": {
                "compile": "unconfirmed",
                "contract": "not_checked",
                "numerical": "not_checked",
            },
            **error_payload(
                "verify_runner", exc, case=args.case, patch=args.apply_patch
            ),
        }
    try:
        write_json(Path(args.json_out), data)
    except OSError as exc:
        # A full disk must not lose the only pointer to an already running build.
        print(
            json.dumps(
                {
                    "status": "report_write_failed",
                    "error_type": type(exc).__name__,
                    "build_invoked": data.get("build_invoked"),
                    "recovery": data.get("recovery"),
                    "result_path": str(args.json_out),
                },
                ensure_ascii=False,
            ),
            file=sys.stderr,
        )
        return 1
    print(str(Path(args.json_out)))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
