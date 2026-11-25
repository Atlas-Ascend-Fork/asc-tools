# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
from __future__ import annotations

import argparse
import json
import os
import re
import secrets
import shutil
import time
from pathlib import Path
from typing import Any

from harness.engine import verify_runner
from harness.engine.build_context_collector import collect_context
from harness.engine.cause_tree import build_cause_tree
from harness.engine.common import error_payload
from harness.engine.common import force_utf8_stdio
from harness.engine.common import load_json
from harness.engine.common import write_json
from harness.engine.compile_repair_guidance import suggest
from harness.engine.compiler_probe_runner import run_probes
from harness.engine.diagnosis_engine import diagnose
from harness.engine.diagnostic_facts import build_diagnostic_facts
from harness.engine.env_probe import collect_tools
from harness.engine.env_probe import detect_cann
from harness.engine.log_normalizer import normalize_log_text
from harness.engine.report_writer import write_report
from harness.engine.stage_evidence import build_stage_evidence
from harness.engine.static_rule_checker import check_context
from harness.engine.verification_evidence import failure_evidence
from harness.engine.version_gate import KB_CANN_BASELINE
from harness.engine.version_gate import is_mismatch
from harness.engine.version_gate import mismatch_next_actions
from harness.engine.version_gate import classify as classify_version


def run_bundle(
    project: Path,
    build_log: Path,
    out: Path,
    soc: str | None = None,
    run_probes_flag: bool = False,
    verify: bool = False,
    apply_patch: Path | None = None,
    verify_sandbox: bool = False,
    verify_prefer_fix_patch: bool = False,
    probe_timeout_s: float = 120.0,
    context_mode: str = "full",
    build_dir: Path | None = None,
) -> dict[str, Any]:
    if context_mode == "light":
        if run_probes_flag or verify or apply_patch:
            raise ValueError(
                "light diagnosis is read-only; choose --context-mode focused/full for probes or verify"
            )
        return run_light_bundle(project, build_log, out, soc)
    out.mkdir(parents=True, exist_ok=True)
    logs_dir = out / "logs"
    logs_dir.mkdir(exist_ok=True)
    if build_log.exists():
        shutil.copy2(build_log, logs_dir / "build.log")
        log_text = build_log.read_text(encoding="utf-8", errors="replace")
    else:
        log_text = ""

    env = {
        "cann": detect_cann(),
        "tools": collect_tools(),
        "npu": {
            "status": "not_requested",
            "reason": "compile_diagnosis_does_not_require_device",
        },
        "warnings": [] if build_log.exists() else [f"build log not found: {build_log}"],
    }
    if soc:
        env["soc"] = soc
    write_json(out / "env.json", env)

    normalized = normalize_log_text(log_text)
    normalized["log_path"] = str(build_log)

    # It may contain absolute paths from the original environment, including
    # unexpanded variables such as $HOME or $CANN_VERSION. Re-anchor paths that
    # do not exist in the current project before exposing them in the report.
    cann_root = env["cann"].get("install_dir")
    echo_snapshots = _reanchor_paths(
        normalized,
        project,
        external_roots=[cann_root] if cann_root else [],
        log_text=log_text,
    )

    context = collect_context(
        project,
        primary_error=normalized.get("primary_error"),
        cann=env["cann"],
        focused=context_mode == "focused",
        build_dir=build_dir,
    )
    for entry, snapshot in echo_snapshots:
        try:
            relative = (
                Path(entry["file"]).relative_to(Path(project).resolve()).as_posix()
            )
        except ValueError:
            relative = ""
        if context.get("file_text", {}).get(relative) != snapshot:
            entry["file"] = (
                str(entry["file_original"]).replace("\\", "/").rsplit("/", 1)[-1]
            )
            entry["file_resolved"] = False
            entry.pop("file_reanchor_method", None)
            entry.pop("file_reanchor_source_sha256", None)
            if entry is normalized.get("primary_error"):
                context.setdefault("source_coverage", {})["failed_source_loaded"] = (
                    False
                )
            context.setdefault("warnings", []).append(
                "Compiler-echo reading candidate changed or was not loaded; its path remains unresolved."
            )
    # Snapshots are transient comparison inputs, never normalized/report data.
    del echo_snapshots
    write_json(out / "normalized.json", normalized)
    write_json(out / "context.json", context)
    write_json(
        out / "failure_evidence.json",
        failure_evidence(project, normalized, target=soc)
        if context_mode == "full"
        else {
            "schema_version": "2.0",
            "status": normalized["status"],
            "source_snapshot": "not_requested",
            "log": normalized["input_evidence"],
        },
    )

    static_findings = check_context(context)
    write_json(out / "static_findings.json", static_findings)

    probes = {
        "probes": [],
        "interpretation": {"likely": "skipped", "confidence": 0.0, "evidence": []},
    }
    if run_probes_flag:
        probes = run_probes(project, context, timeout_s=probe_timeout_s)
    write_json(out / "probes.json", probes)

    diagnosis = diagnose(normalized, context, static_findings, probes)
    diagnosis["context_mode"] = context_mode
    write_json(out / "diagnosis.json", diagnosis)
    if isinstance(diagnosis.get("cause_tree"), dict):
        write_json(out / "cause_tree.json", diagnosis["cause_tree"])

    patch_suggestions = suggest(diagnosis, normalized, project, soc=soc)
    patch_suggestions_path = out / "patch_suggestions.json"
    write_json(patch_suggestions_path, patch_suggestions)

    verify_result: dict[str, Any] = {}
    if verify:
        chosen_patch = apply_patch
        if chosen_patch is None and verify_prefer_fix_patch:
            cand = project / "fix.patch"
            if cand.exists():
                chosen_patch = cand
        try:
            verify_result = verify_runner.verify(
                case_dir=project,
                patch_file=chosen_patch,
                patch_suggestions=patch_suggestions_path,
                soc=soc,
                sandbox=verify_sandbox,
            )
        except Exception as exc:  # noqa: BLE001 - preserve a structured CLI result
            verify_result = {
                "before": {},
                "after": {},
                "transition": "verification_not_run",
                "logs": {},
                "build_invoked": False,
                **error_payload("verify_runner", exc, project=str(project)),
            }
        write_json(out / "verify.json", verify_result)

    # 此前恒传 {}：report.md「验证结果」节在 --verify 真跑过时也渲染"未运行验证"。
    write_report(
        project,
        diagnosis,
        patch_suggestions,
        verify_result,
        out / "report.md",
        probes=probes,
        env=env,
    )
    bundle: dict[str, Any] = {
        "env": str(out / "env.json"),
        "normalized": str(out / "normalized.json"),
        "failure_evidence": str(out / "failure_evidence.json"),
        "context": str(out / "context.json"),
        "static_findings": str(out / "static_findings.json"),
        "probes": str(out / "probes.json"),
        "diagnosis": str(out / "diagnosis.json"),
        "patch_suggestions": str(patch_suggestions_path),
        "report": str(out / "report.md"),
    }
    if verify:
        bundle["verify"] = str(out / "verify.json")
    if (out / "cause_tree.json").exists():
        bundle["cause_tree"] = str(out / "cause_tree.json")
    return bundle


def run_light_bundle(
    project: Path, build_log: Path, out: Path, soc: str | None = None
) -> dict:
    """Log-only default entry. Source and environment reads require inspect."""
    if not build_log.is_file():
        raise FileNotFoundError(f"build log not found: {build_log}")
    out.mkdir(parents=True, exist_ok=True)
    raw = build_log.read_bytes()
    normalized = normalize_log_text(raw.decode("utf-8", errors="replace"))
    import hashlib

    normalized["input_evidence"]["sha256"] = hashlib.sha256(raw).hexdigest()
    normalized["log_path"] = str(build_log)
    normalized["context_mode"] = "light"
    primary = normalized.get("primary_error") or {}
    query_input = dict(normalized)
    if soc is not None:
        # Keep the supplied target; a log target is independent observed evidence.
        query_input["target_soc"] = soc
    legacy_phase = primary.get("phase", "unknown")
    diagnostic_facts = build_diagnostic_facts(normalized, {})
    stage_evidence = build_stage_evidence(normalized, {}, legacy_phase=legacy_phase)
    diagnosis = {
        "schema_version": "2.0",
        "context_mode": "light",
        "status": normalized["status"],
        "summary": primary.get("message")
        or "No recognized failure; execution outcome remains unknown.",
        "category": primary.get("category", "unknown"),
        "phase": legacy_phase,
        "diagnostic_facts": diagnostic_facts,
        **stage_evidence,
        "primary_error": primary or None,
        "self_explanatory": False,
        "root_cause_candidates": [],
        "cause_tree": build_cause_tree(
            [],
            category=primary.get("category", "unknown"),
            phase=legacy_phase,
            normalized=query_input,
        ),
        "next_actions": [
            "Read the failed source and its relevant declaration or effective command as needed.",
            "Preserve the requested target and behavior; verify the original recipe after a supported edit.",
        ]
        if primary
        else ["Obtain the actual exit receipt or a complete failing log."],
        "coverage": {
            "source_files_read": 0,
            "environment_probed": False,
            "build_invoked": False,
        },
    }
    patch_suggestions = suggest(diagnosis, normalized, project, soc=soc)
    values = {
        "normalized": normalized,
        "diagnosis": diagnosis,
        "context": {
            "case_dir": str(project),
            "context_mode": "light",
            "file_text": {},
            "files": {},
            "source_coverage": {"loaded": 0, "scope": "not_requested"},
        },
        "env": {
            "probe_status": "not_requested",
            "cann": {"found": None, "version": "unknown", "status": "not_requested"},
            "tools": {},
            "soc": soc,
        },
        "patch_suggestions": patch_suggestions,
    }
    paths = {}
    for name, data in values.items():
        path = out / f"{name}.json"
        write_json(path, data)
        paths[name] = str(path)
    (out / "logs").mkdir(exist_ok=True)
    (out / "logs/build.log").write_bytes(raw)
    report = out / "report.md"
    report.write_text(
        "# Compile diagnosis\n\n"
        + diagnosis["summary"]
        + "\n\nLog parsed; source/environment not inspected. Build outcome and repair remain unverified.\n",
        encoding="utf-8",
    )
    paths["report"] = str(report)
    return paths


#
# 设计：bundle 已落盘后再读 JSON 拼摘要。
# 这样错误路径（run_bundle 抛异常前/中）与正常路径共用同一份 stdout schema。
# 这里若 verify.json 缺失则填占位 build_invoked=false，与验证证据检查口径对齐。


def generate_run_id(now: float | None = None) -> str:
    """生成可读 run_id：UTC 时间戳 + 4 字节随机。"""
    epoch = now if now is not None else time.time()
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime(epoch))
    return f"{stamp}-{secrets.token_hex(4)}"


def _reanchor_one(raw: str, project: Path, *, external_roots=()) -> tuple[str, bool]:
    """Return an owned current reading candidate, or an unresolved display name."""
    from harness.engine.common import resolve_project_source

    resolved = resolve_project_source(
        raw, project, reanchor=True, external_roots=external_roots
    )
    if resolved is not None:
        return str(resolved), True
    norm = raw.replace("\\", "/").strip()
    return norm.rsplit("/", 1)[-1] or norm, False


def _validated_source_echo(entry: dict[str, Any], lines: list[str]) -> str | None:
    from harness.engine.common import _source_echo_has_anchor

    position, echo = entry.get("log_line"), entry.get("code_line")
    if (
        entry.get("kind") not in {"error", "fatal_error"}
        or not isinstance(position, int)
        or not isinstance(echo, str)
        or not 0 < position < len(lines) - 1
    ):
        return None
    reported, source, caret = lines[position - 1 : position + 2]
    if reported.strip() != entry.get("raw") or source.strip(" \t") != echo:
        return None
    if "\t" in source or "\t" in caret or not re.fullmatch(r"[ ~]*\^[ ~]*", caret):
        return None
    column = caret.index("^") + 1
    if entry.get("column") is not None and entry["column"] != column:
        return None
    indentation = len(source) - len(source.lstrip(" "))
    return (
        echo
        if _source_echo_has_anchor(echo, entry.get("message"), column - indentation)
        else None
    )


def _reanchor_paths(
    normalized: dict[str, Any],
    project: Path,
    *,
    external_roots=(),
    log_text: str | None = None,
) -> list[tuple[dict[str, Any], str]]:
    """Re-anchor normalized diagnostic paths to readable files in the current project."""
    try:
        project = Path(project)
    except TypeError:
        return []
    snapshots = {}
    lines = log_text.splitlines() if isinstance(log_text, str) else []

    def fix(entry: Any, *, error: bool = False) -> None:
        from harness.engine.common import resolve_project_source

        if not isinstance(entry, dict):
            return
        raw = entry.get("file_original") or entry.get("file")
        if not isinstance(raw, str) or not raw:
            return
        # Recompute this call's inference; an earlier echo hash must not survive
        # a changed tree that now selects a different directory-based candidate.
        entry.pop("file_reanchor_method", None)
        entry.pop("file_reanchor_source_sha256", None)
        snapshots.pop(id(entry), None)
        direct = resolve_project_source(raw, project)
        new, ok = _reanchor_one(raw, project, external_roots=external_roots)
        echo = _validated_source_echo(entry, lines) if error and not ok else None
        evidence = {}
        if echo is not None:
            candidate = resolve_project_source(
                raw,
                project,
                reanchor=True,
                external_roots=external_roots,
                source_line=entry.get("line"),
                source_echo=echo,
                diagnostic_message=entry.get("message"),
                source_evidence=evidence,
            )
            if candidate is not None and evidence:
                new, ok = str(candidate), True
                entry["file_reanchor_method"] = "compiler_source_echo"
                entry["file_reanchor_source_sha256"] = evidence["source_sha256"]
                snapshots[id(entry)] = (entry, evidence["source_text"])
        if not ok:
            entry.pop("file_reanchor_method", None)
            entry.pop("file_reanchor_source_sha256", None)
            snapshots.pop(id(entry), None)
        entry.setdefault(
            "file_original", raw
        )  # External declarations remain auditable log evidence.
        entry["file"] = new
        entry["file_resolved"] = ok
        # A readable current candidate is not proof the historical error came
        # from that source. Preserve the marker when shared entries are visited twice.
        entry["file_reanchored"] = bool(entry.get("file_reanchored")) or bool(
            ok and direct is None
        )

    fix(normalized.get("primary_error"), error=True)
    for key in ("all_errors", "downstream_errors", "notes", "include_chain"):
        seq = normalized.get(key)
        if isinstance(seq, list):
            for item in seq:
                fix(item, error=key in {"all_errors", "downstream_errors"})
    return list(snapshots.values())


def _build_environment_checks(
    normalized_data: dict[str, Any], env_data: dict[str, Any], requested_soc: str | None
) -> list[dict[str, str]]:
    """Build environment-consistency findings from recorded evidence without probing or changing the project."""
    checks: list[dict[str, str]] = []
    # ① SoC 三方对照
    log_soc = normalized_data.get("target_soc")
    env_npu = (env_data.get("npu") or {}).get("devices") or []
    first = env_npu[0] if env_npu else {}
    npu_soc = (
        first.get("soc")
        if isinstance(first, dict)
        else (first if isinstance(first, str) else None)
    )
    socs = {"requested": requested_soc, "log": log_soc, "device": npu_soc}
    known = {k: v for k, v in socs.items() if v}
    vals = set(known.values())
    if len(vals) > 1:
        checks.append(
            {
                "check": "soc_consistency",
                "status": "warn",
                "detail": f"SoC 三方不一致：{known}——先确认编译目标到底是谁，再谈修法",
            }
        )
    elif len(vals) == 1:
        checks.append(
            {
                "check": "soc_consistency",
                "status": "ok",
                "detail": f"SoC 一致：{next(iter(vals))}（{list(known)}）",
            }
        )
    else:
        checks.append(
            {"check": "soc_consistency", "status": "unknown", "detail": "无 SoC 证词"}
        )
    if env_data.get("probe_status") == "not_requested":
        checks.extend(
            {"check": name, "status": "unknown", "detail": "not_requested"}
            for name in ("toolkit_path_consistency", "build_tools")
        )
        return checks
    # ② toolkit 路径一致性
    _cann = env_data.get("cann") or {}
    inst = _cann.get("install_dir")
    ahp = os.environ.get("ASCEND_HOME_PATH")
    if inst and ahp and Path(ahp).resolve() != Path(inst).resolve():
        checks.append(
            {
                "check": "toolkit_path_consistency",
                "status": "warn",
                "detail": f"ASCEND_HOME_PATH={ahp} 与探测到的 CANN 安装 {inst} 不一致——verify 可能编错 toolkit",
            }
        )
    elif inst:
        checks.append(
            {
                "check": "toolkit_path_consistency",
                "status": "ok",
                "detail": f"CANN: {inst}",
            }
        )
    else:
        checks.append(
            {
                "check": "toolkit_path_consistency",
                "status": "warn",
                "detail": "本机未探得 CANN 安装（诊断可用；真编验证不可用）",
            }
        )
    # ③ 构建工具
    tools = env_data.get("tools") or {}
    missing = [k for k in ("cmake",) if not tools.get(k)]
    checks.append(
        {
            "check": "build_tools",
            "status": "warn" if missing else "ok",
            "detail": f"缺失：{missing}" if missing else "cmake 可用",
        }
    )
    # ④ 跨案环境变量残留（catlass 泄漏）
    leaks = [
        k
        for k in (
            "ASCENDC_CATLASS_ROOT",
            "ASCENDC_CATLASS_EXAMPLE",
            "ASCENDC_CATLASS_ARCH",
        )
        if os.environ.get(k)
    ]
    if leaks:
        checks.append(
            {
                "check": "env_var_leak",
                "status": "warn",
                "detail": f"{leaks} 在环境里——catlass 定向变量会把 verify 钉到特定 example/arch（跨案残留=假信号温床），非本意请 unset",
            }
        )
    return checks


def _build_environment_checks_guard() -> None:  # pragma: no cover - 文档锚点
    pass


def _build_fast_path(primary_error: dict[str, Any] | None) -> dict[str, Any] | None:
    """Expose the compiler's spelling candidate without inferring rename direction.

    ``safe_to_apply`` is a spelling heuristic, not evidence of type or semantic
    equivalence. Definition/use contracts determine which side needs a change.
    """
    if not primary_error:
        return None
    dym = primary_error.get("did_you_mean")
    if not dym:
        return None
    wrong, right = dym.get("wrong_symbol"), dym.get("suggested_symbol")
    loc = dym.get("file") or primary_error.get("file")
    line = dym.get("line") or primary_error.get("line")
    evidence = {
        "applicability": "unknown",
        "applicability_reasons": [
            "compiler_spelling_suggestion_only",
            "semantic_equivalence_unverified",
        ],
        "source_provenance": {
            "kind": "compiler_diagnostic",
            "current_diagnostic": {
                k: primary_error.get(k) for k in ("message", "raw", "file", "line")
            },
            "suggestion": {"from": wrong, "to": right, "file": loc, "line": line},
        },
        "then": (
            "已有证据足够时直接局部修复，沿原构建 recipe 真实编译，可用 run_verify(case_dir=<project_dir>)；"
            "compile_passed / compile_failure_fixed 只完成编译层，按任务继续核验接口契约和数值；"
            "若出现新错误，把新的完整日志回喂诊断。"
        ),
    }
    if dym.get("safe_to_apply"):
        return {
            **evidence,
            "kind": "compiler_suggested_rename",
            "confidence": "medium",
            "action": (
                f"候选：在 {loc or '首错文件'} 第 {line or '?'} 行将 `{wrong}` 改为 `{right}`；"
                "已有声明、使用点和需求足以确认类型与语义相容时可直接采用；"
                "证据不足或冲突时只补查缺少的定义、调用或目标信息。"
            ),
            "replace": {"from": wrong, "to": right, "file": loc, "line": line},
            "do_not": "同名异型警示：近名建议不证明类型或语义等价；声明、使用点或需求反证时撤回该候选。",
        }
    return {
        **evidence,
        "kind": "needs_localization",
        "confidence": "low",
        "warning": (
            f"编译器建议把 `{wrong}` 改成 `{right}`；不要照抄建议或仅凭名字后缀认定定义被改坏。"
            "应由定义、使用点与原始需求决定改哪一侧。"
        ),
        "action": (
            f"用已有的 `{right}` 与 `{wrong}` 定义、使用点和需求判断应修定义还是使用点；"
            "已有证据足够时直接局部修复，仅在不足或冲突时补查对应声明与调用链。"
        ),
    }


def _fast_path_next_actions(fast_path: dict[str, Any]) -> list[str] | None:
    """Keep candidate instructions and the top-level workflow consistent.

    Existing evidence can justify an immediate local edit. Only missing or
    conflicting facts require another read; verification proves the compile layer.
    Unknown kinds retain the engine's fallback workflow.
    """
    kind = fast_path.get("kind")
    evidence_step = (
        "已有声明、调用、目标和需求证据足够时直接局部修复；仅在证据不足或冲突时，"
        "补查所缺的最小声明、目标或调用信息（可查源码及参考资料）。"
        "若源码或当前工具链反证候选，撤回候选并据真实证据修正。"
    )
    verify_step = (
        "改后沿原构建 recipe 真实编译，可用 run_verify(case_dir=<project_dir>)；"
        "compile_passed / compile_failure_fixed 只完成编译层，按任务继续核验接口契约和数值。"
        "若出现新错误，把新的完整日志回喂诊断。"
    )
    if kind == "compiler_suggested_rename":
        rep = fast_path.get("replace") or {}
        frm, to = rep.get("from"), rep.get("to")
        where = rep.get("file")
        line = rep.get("line")
        locus = f"{where}:{line}" if where and line else (where or "首错文件")
        acts = [
            (
                f"改名候选：`{frm}` → `{to}`（{locus}，见 fast_path.replace）；"
                "采用前以已有定义、使用点和需求判断类型与语义是否相容。"
            ),
        ]
        acts.extend([evidence_step, verify_step])
        return acts
    if kind == "needs_localization":
        return [
            fast_path.get("warning") or "编译器近名建议不能决定应改定义还是使用点。",
            fast_path.get("action")
            or "用定义、使用点和原始需求判断应修定义还是使用点。",
            evidence_step,
            verify_step,
        ]
    return None


def _summarize_npu(npu: dict[str, Any]) -> dict[str, Any]:
    """Keep an unrequested or inconclusive device probe distinct from absence."""
    status = npu.get("status")
    devices = npu.get("devices")
    count = None
    if status == "not_requested":
        probe_status = "not_requested"
    elif npu.get("npu_smi_available") is False or status == "unavailable":
        probe_status = "unavailable"
    elif npu.get("info_returncode") not in (None, 0) or status == "failed":
        probe_status = "failed"
    elif isinstance(devices, list) and devices:
        probe_status = "observed"
        count = len(devices)
    elif isinstance(devices, list):
        # The probe extracts matching npu-smi lines. No match does not establish
        # that the machine has no devices, even when the command exits zero.
        probe_status = "inconclusive"
    else:
        probe_status = "unknown"
    return {"npu_devices": count, "npu_probe_status": probe_status}


def build_stdout_summary(
    out: Path, files: dict[str, str], run_id: str
) -> dict[str, Any]:
    """从落盘 bundle 读出关键字段，构造 --json-stdout 单行 JSON。"""
    diagnosis = (
        load_json(Path(files.get("diagnosis", out / "diagnosis.json")), default={})
        or {}
    )
    patches = (
        load_json(
            Path(files.get("patch_suggestions", out / "patch_suggestions.json")),
            default={},
        )
        or {}
    )
    suggestions = patches.get("suggestions") or []
    top = suggestions[0] if suggestions else None
    verify_path = out / "verify.json"
    if verify_path.exists():
        verify = load_json(verify_path, default={}) or {}
    else:
        verify = {"build_invoked": False, "reason": "verify_not_run"}

    # Keep the toolchain and target visible in the compact diagnosis summary.
    env_data = load_json(out / "env.json", default={}) or {}
    _cann = env_data.get("cann") or {}
    # Compare the failing build's logged version with the locally probed version.
    # version_gate owns the compatibility policy and knowledge applicability.
    normalized_data = load_json(out / "normalized.json", default={}) or {}
    version_compat = classify_version(
        normalized_data.get("cann_version"), _cann.get("version")
    )
    version_mismatch = is_mismatch(version_compat)
    summary_env = {
        "cann_found": _cann.get("found"),
        "cann_version": _cann.get("version"),
        "cann_install_dir": _cann.get("install_dir"),
        "soc": env_data.get("soc"),
        **_summarize_npu(env_data.get("npu") or {}),
    }
    if env_data.get("warnings"):
        summary_env["warnings"] = env_data["warnings"]

    # Keep root-cause localization in the compact summary so consumers can
    # inspect the likely root file and verification can preserve the evidence.
    root_localization = diagnosis.get("root_cause_localization")

    # next_actions 给引擎建议步骤——都是调用方定向修复的关键，旧 summary 也漏了。primary_error
    # Keep the compact summary small enough for automated callers.
    pe = diagnosis.get("primary_error") or {}
    primary_error = (
        {
            "category": pe.get("category"),
            "message": pe.get("message"),
            "file": pe.get("file"),
            "line": pe.get("line"),
            "log_line": pe.get("log_line"),
            "original_log_span": pe.get("original_log_span"),
            # The display path may differ from the resolved project path; retain both fields.
            # A resolved path is evidence of readability, not proof of historical identity.
            "file_resolved": pe.get("file_resolved"),
            "file_original": pe.get("file_original"),
            "file_reanchored": pe.get("file_reanchored"),
            **{
                key: pe[key]
                for key in ("file_reanchor_method", "file_reanchor_source_sha256")
                if key in pe
            },
        }
        if pe
        else None
    )
    # Preserve the compiler's suggestion and locus; naming heuristics cannot
    # establish which definition or use conforms to the original contract.
    fast_path = _build_fast_path(pe)
    # A compiler's nearby name does not resolve an API/template contract.
    # Keep simple spelling fixes fast, but defer domain-sensitive substitutions
    # until the host has inspected the current declaration and call semantics.
    contract_candidates = [
        leaf
        for leaf in (diagnosis.get("cause_tree") or {}).get("leaves", [])
        if leaf.get("kind") == "compiler_contract_candidate"
    ]
    if (
        contract_candidates
        and fast_path
        and fast_path.get("kind") == "compiler_suggested_rename"
    ):
        fast_path = None
    if version_mismatch:
        # A known version mismatch suppresses baseline-specific candidates.
        fast_path = None
    # Keep the visible workflow consistent with candidate applicability.
    next_actions = diagnosis.get("next_actions", [])
    if fast_path:
        actions = _fast_path_next_actions(fast_path)
        if actions:
            next_actions = actions
    if version_mismatch:
        # top_suggestion 保留为参考，但"下一步做什么"只剩版本裁决一条路。
        next_actions = mismatch_next_actions(version_compat)

    # Surface unresolved choices without selecting a rename side or deletion.
    # Existing requirements and def/use evidence may already resolve the choice.
    decision_advisory = None
    if not version_mismatch:
        if fast_path and fast_path.get("kind") == "needs_localization":
            decision_advisory = {
                "kind": "intent_conflict_rename",
                "what": "改名方向待定：先由已有需求、定义和使用点证据消歧；仅当必要需求仍未知且影响选择时询问调用者。",
                "options": [
                    "若原始接口契约要求原名且当前定义偏离该契约，修正定义并核对使用点",
                    "若当前定义符合原始接口契约且使用点过期，修正相关使用点",
                ],
                "applicability": fast_path.get("applicability", "unknown"),
                "destructive": True,
                "unattended_default": None,
            }
    # Evaluate environment consistency before presenting repair guidance.
    environment_checks = _build_environment_checks(
        normalized_data=normalized_data,
        env_data=env_data,
        requested_soc=env_data.get("soc"),
    )

    # Keep the compact summary focused on diagnosis and the human-readable report.
    # The complete bundle remains available through its top-level path.
    slim_files = {k: files[k] for k in ("diagnosis", "report") if k in files}
    return {
        "schema_version": "2.0",
        "context_mode": diagnosis.get("context_mode", "full"),
        "observation_status": normalized_data.get("status", "unknown"),
        "log_evidence": normalized_data.get("input_evidence"),
        "bundle": str(out),
        "summary": diagnosis.get("summary", ""),
        "phase": diagnosis.get("phase", ""),
        **{
            key: diagnosis[key]
            for key in (
                "failure_stage",
                "source_context",
                "path_hint",
                "conflicts",
                "diagnostic_facts",
            )
            if key in diagnosis
        },
        "category": diagnosis.get("category", ""),
        "primary_error": primary_error,
        "root_cause_localization": root_localization,
        # A strong architecture gate disables the self-explanatory shortcut.
        # The reported identifier may already be correct for another target.
        "self_explanatory": diagnosis.get("self_explanatory", False),
        # A visible candidate carries applicability and provenance, not a fix verdict.
        "fast_path": fast_path,
        # Pending questions are distinct from executed queries and compiler evidence.
        "knowledge_requests": (diagnosis.get("cause_tree") or {}).get(
            "knowledge_requests", []
        ),
        "diagnostic_guidance": [
            leaf
            for leaf in (diagnosis.get("cause_tree") or {}).get("leaves", [])
            if leaf.get("kind") in {"compiler_contract_candidate", "compiler_probe"}
        ][:3],
        "next_actions": next_actions,
        # Use a gate-specific suggestion instead of generic API-edit advice.
        "top_suggestion": top,
        # Preserve the public version status and expose unknown when the environment cannot be checked.
        "version_compatibility": version_compat,
        "verify": verify,
        "env": summary_env,
        "environment_checks": environment_checks,
        "decision_advisory": decision_advisory,
        "run_id": run_id,
        "files": slim_files,
    }


def _error_stdout_summary(
    out: Path, run_id: str, err: dict[str, Any]
) -> dict[str, Any]:
    """run_bundle 异常时的 stdout 兜底：保留同一 schema，关键字段空但 error 非空。"""
    return {
        "bundle": str(out),
        "summary": err.get("summary", "diagnose_compile_failure failed"),
        "phase": "",
        "category": "",
        "top_suggestion": None,
        "version_compatibility": {
            "kb_baseline": KB_CANN_BASELINE,
            "detected": None,
            "source": "none",
            "status": "unknown",
        },
        "verify": {"build_invoked": False, "reason": "diagnose_failed"},
        "run_id": run_id,
        "files": {"diagnosis": str(out / "diagnosis.json")},
        "error": err.get("error"),
    }


def main() -> int:
    force_utf8_stdio()  # Keep structured output lossless on non-UTF-8 consoles.
    parser = argparse.ArgumentParser(
        description="One-shot Ascend C compile-failure diagnosis bundle."
    )
    parser.add_argument("--project", required=True)
    parser.add_argument("--build-log", required=True)
    parser.add_argument("--soc")
    parser.add_argument("--out", required=True)
    parser.add_argument("--run-probes", action="store_true")
    parser.add_argument(
        "--context-mode",
        choices=("light", "focused", "full"),
        default="light",
        help="Default parses only the log; full retains the legacy bundle API.",
    )
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument(
        "--probe-timeout",
        type=float,
        default=120.0,
        help="每个 compiler_probe_runner probe 的超时时间（秒）；仅在 --run-probes 时生效。",
    )
    parser.add_argument(
        "--json-stdout",
        action="store_true",
        help="Write the compact bundle summary as one JSON line for callers and CI.",
    )
    parser.add_argument(
        "--verify",
        action="store_true",
        help="Run verification and write the result to bundle/verify.json.",
    )
    parser.add_argument(
        "--apply-patch",
        help="Patch file for verification (unified diff or ---env--- header); defaults to patch_suggestions[0].",
    )
    parser.add_argument(
        "--verify-sandbox",
        action="store_true",
        help="Run verification inside firejail/unshare -rn when available.",
    )
    parser.add_argument(
        "--verify-prefer-fix-patch",
        action="store_true",
        help="Prefer project/fix.patch when it exists.",
    )
    args = parser.parse_args()
    run_id = generate_run_id()
    out_path = Path(args.out).resolve()
    try:
        bundle = run_bundle(
            Path(args.project).resolve(),
            Path(args.build_log).resolve(),
            out_path,
            args.soc,
            args.run_probes,
            verify=args.verify,
            apply_patch=Path(args.apply_patch).resolve() if args.apply_patch else None,
            verify_sandbox=args.verify_sandbox,
            verify_prefer_fix_patch=args.verify_prefer_fix_patch,
            probe_timeout_s=args.probe_timeout,
            context_mode=args.context_mode,
            build_dir=args.build_dir,
        )
        if args.json_stdout:
            summary = build_stdout_summary(out_path, bundle, run_id)
            summary["schema_version"] = "2.0"
            summary["context_mode"] = args.context_mode
            summary["observation_status"] = load_json(out_path / "normalized.json").get(
                "status"
            )
            print(json.dumps(summary, ensure_ascii=False))
        else:
            print(bundle["report"])
        return 0
    except Exception as exc:  # noqa: BLE001 - CLI must always emit a failure artifact
        out_path.mkdir(parents=True, exist_ok=True)
        err = {
            "summary": "diagnose_compile_failure failed",
            **error_payload("diagnose_compile_failure", exc),
        }
        write_json(out_path / "diagnosis.json", err)
        if args.json_stdout:
            print(
                json.dumps(
                    _error_stdout_summary(out_path, run_id, err), ensure_ascii=False
                )
            )
        else:
            print(str(out_path / "diagnosis.json"))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
