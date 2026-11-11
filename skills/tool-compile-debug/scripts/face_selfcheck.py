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
"""Check that an installed package is complete, readable, and clean.

The check is read-only: it does not invoke a compiler or access the network.
Use ``--json`` to obtain the file fingerprints collected during the check.

用途：安装后自检——必需文件齐全、包内文件非空、无运行期污染、引擎可独立导入。
使用方法：
    python3 face_selfcheck.py          # 人读结果
    python3 face_selfcheck.py --json   # 含文件指纹的机器可读结果
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from collections.abc import Iterator
from pathlib import Path
from typing import Any

FACE = Path(__file__).resolve().parent.parent
sys.dont_write_bytecode = True

ENGINE_MODULES = (
    "__init__",
    "common",
    "source_facts",
    "response_files",
    "inspect_context",
    "execution",
    "verification_recovery",
    "repair_memory",
    "audit",
    "version_gate",
    "build_context_collector",
    "env_probe",
    "compiler_probe_runner",
    "diagnosis_engine",
    "cause_tree",
    "root_localizer",
    "log_normalizer",
    "compile_repair_guidance",
    "report_writer",
    "static_rule_checker",
    "verify_runner",
    "redactor",
    "diagnose_compile_failure",
    "verification_evidence",
    "build_receipts",
    "stage_evidence",
    "diagnostic_facts",
)
CONFIG_FILES = (
    "compile_mechanisms",
    # api_signatures 不在此列：包内允许两种形态（单文件 harness/knowledge/api_signatures.yaml，
    # 或 harness/knowledge/api_signatures/part-*.yaml 分片目录），由 inspect_face 内的专门检查覆盖。
    "cann_error_codes",
    "fix_hints",
    "api_version_map",
    "soc_feature_matrix",
    "operator_idiom_kb",
    "static_rules",
    "error_signatures",
    "fix_templates",
    "reserved_identifiers",
    "versions",
    "kb_meta",
)

REQUIRED_FILES = (
    "SKILL.md",
    # 技能根不带 README.md / requirements.txt（宿主仓技能惯例；
    # 运行依赖在 SKILL.md 环境要求中声明）。
    "evals/evals.json",
    "evals/trigger-evals.json",
    "scripts/run.py",
    "scripts/preflight.py",
    "scripts/face_selfcheck.py",
    "scripts/update_kb_manifest.py",
    "scripts/diagnose_compile_failure.sh",
    "scripts/run_verify.sh",
    "scripts/redact_bundle.sh",
    "scripts/engine/__init__.py",
    "scripts/engine/README.md",
    "scripts/engine/harness/__init__.py",
    "references/using-scripts.md",
    "references/maintenance.md",
    "references/trigger-evaluation.md",
    "references/json-schemas.md",
    "references/error-categories/README.md",
    "references/troubleshooting.md",
    "scripts/engine/harness/runtime/build_one.sh",
    "scripts/engine/harness/engine/probe_msopgen_regenerate_poc.py",
    # 脚本的单元测试位于技能根 tests/（结构测试另钉本清单）
    "tests/test_structure.py",
    "tests/test_evals.py",
    "tests/test_scripts.py",
    "tests/test_kb_shards.py",
    "tests/test_kb_maintenance.py",
    "tests/test_engine_behavior.py",
    "tests/test_trigger_evals.py",
    *(f"scripts/engine/harness/engine/{name}.py" for name in ENGINE_MODULES),
    *(f"scripts/engine/harness/knowledge/{name}.yaml" for name in CONFIG_FILES),
)


def _package_paths(face: Path) -> Iterator[Path]:
    """Visit package contents without reading root Git repository metadata."""
    for path in face.iterdir():
        if path.name == ".git":
            continue
        yield path
        if path.is_dir():
            yield from path.rglob("*")


def read_skill_version(face: Path, problems: list[str]) -> str | None:
    """Read the release identity from the skill's single metadata source."""
    try:
        import yaml
    except ImportError as exc:
        problems.append(f"SKILL.md metadata.version check unavailable: {exc}")
        return None
    try:
        lines = (face / "SKILL.md").read_text(encoding="utf-8").splitlines()
        if not lines or lines[0] != "---":
            raise ValueError("missing YAML frontmatter")
        header = yaml.safe_load("\n".join(lines[1 : lines.index("---", 1)]))
        metadata = header.get("metadata") if isinstance(header, dict) else None
        version = metadata.get("version") if isinstance(metadata, dict) else None
        if not isinstance(version, str) or not re.fullmatch(
            r"[0-9]+\.[0-9]+(?:\.[0-9]+)?", version
        ):
            raise ValueError("expected a string major.minor[.patch] release version")
        return version
    except (OSError, UnicodeError, ValueError, yaml.YAMLError) as exc:
        problems.append(f"SKILL.md metadata.version check failed: {exc}")
        return None


def inspect_face(face: Path = FACE) -> dict[str, Any]:
    """Return required-file checks and SHA-256 fingerprints for ``face``."""
    problems: list[str] = []
    for relative in REQUIRED_FILES:
        path = face / relative
        try:
            if not path.is_file():
                problems.append(f"missing required file: {relative}")
        except OSError as exc:
            problems.append(f"unreadable required file: {relative}: {exc}")

    # api_signatures 双形态：单文件，或 api_signatures/part-*.yaml 分片（解析等价）。
    signature_single = (
        face / "scripts" / "engine" / "harness/knowledge" / "api_signatures.yaml"
    )
    signature_dir = face / "scripts" / "engine" / "harness/knowledge" / "api_signatures"
    signature_parts = (
        sorted(signature_dir.glob("part-*.yaml")) if signature_dir.is_dir() else []
    )
    if not signature_single.is_file() and not signature_parts:
        problems.append(
            "missing required file: scripts/engine/harness/knowledge/api_signatures.yaml"
            " (single file or api_signatures/part-*.yaml shards)"
        )
    elif (
        not signature_single.is_file()
        and not (signature_dir / "manifest.json").is_file()
    ):
        problems.append(
            "missing API signature shard manifest: harness/knowledge/api_signatures/manifest.json"
        )

    for runtime_dir in (face / "scripts" / "engine" / "workspace", face / "workspace"):
        if runtime_dir.is_dir():
            problems.append(
                f"runtime artifacts inside installed package: {runtime_dir.relative_to(face)}"
            )

    try:
        paths = list(_package_paths(face)) if face.is_dir() else []
    except OSError as exc:
        problems.append(f"package traversal failed: {exc}")
        paths = []
    for path in paths:
        if "__pycache__" in path.parts or path.suffix == ".pyc":
            problems.append(
                f"bytecode artifact inside installed package: {path.relative_to(face)}"
            )
            break

    fingerprints: dict[str, str] = {}
    for path in sorted(paths):
        if path.is_file() and "__pycache__" not in path.parts:
            try:
                relative = path.relative_to(face).as_posix()
                content = path.read_bytes()
                if not content.strip():
                    problems.append(
                        f"empty or whitespace-only package file: {relative}"
                    )
                fingerprints[relative] = hashlib.sha256(content).hexdigest()
            except OSError as exc:
                problems.append(
                    f"unreadable package file: {path.relative_to(face)}: {exc}"
                )

    return {
        "ok": not problems,
        "skill_version": None,
        "errors": problems,
        "files": fingerprints,
        "file_count": len(fingerprints),
        "required_count": len(REQUIRED_FILES),
    }


def _check_engine_imports(result: dict[str, Any]) -> None:
    """Import the package closure and verify the API knowledge-base integrity."""
    if result["errors"]:
        return
    result["skill_version"] = read_skill_version(FACE, result["errors"])
    if result["errors"]:
        return
    sys.path.insert(0, str(FACE / "scripts" / "engine"))
    try:
        from harness.engine.common import PROJECT_ROOT
        from harness.engine.common import load_api_signature_kb
        from harness.engine.common import load_yaml
        from harness.engine.diagnose_compile_failure import run_bundle  # noqa: F401
        from harness.engine.redactor import main as redact_main  # noqa: F401
        from harness.engine.verify_runner import verify  # noqa: F401

        if Path(PROJECT_ROOT).resolve() != (FACE / "scripts" / "engine").resolve():
            result["errors"].append(
                "A different skills package is already imported; run self-check "
                "in a fresh Python process."
            )
            return

        api_signatures = load_api_signature_kb(
            FACE / "scripts" / "engine" / "harness/knowledge"
        )
        if not api_signatures:
            result["errors"].append("API signature knowledge base contains no entries")

        signatures = (
            load_yaml(
                FACE
                / "scripts"
                / "engine"
                / "harness/knowledge"
                / "error_signatures.yaml",
                default=[],
            )
            or []
        )
        if not signatures:
            result["errors"].append("error_signatures.yaml contains no rules")
    except Exception as exc:  # noqa: BLE001
        result["errors"].append(
            f"engine or knowledge-base check failed: {type(exc).__name__}: {exc}"
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print checks and SHA-256 fingerprints as JSON.",
    )
    arguments = parser.parse_args(argv)
    result = inspect_face()
    _check_engine_imports(result)
    result["ok"] = not result["errors"]

    if arguments.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if result["ok"] else 1
    if not result["ok"]:
        print(
            "[face_selfcheck] FAIL - package is incomplete or polluted", file=sys.stderr
        )
        for problem in result["errors"]:
            print(f"  - {problem}", file=sys.stderr)
        print(
            "Reinstall the complete package directory and run the check again.",
            file=sys.stderr,
        )
        return 1
    print(
        f"[face_selfcheck] OK - tool-compile-debug {result['skill_version']}; "
        f"{result['required_count']} required paths; "
        f"{result['file_count']} files; engine imports OK. Use --json for SHA-256 fingerprints."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
