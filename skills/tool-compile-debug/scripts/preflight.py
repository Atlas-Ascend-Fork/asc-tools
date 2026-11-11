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
"""Check package integrity and local capabilities before use.

用途：使用前检查包完整性与本机能力（引擎可载、知识库在位、平台支持度）。
使用方法：
    python3 preflight.py          # 人读摘要
    python3 preflight.py --json   # 机器可读结果
The command verifies the bundled engine, its knowledge base, and the Python
runtime. A missing CANN compiler is reported as a warning because diagnosis
does not require compilation; verification of a fix does.

Usage: ``python preflight.py [--json]``
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve()
ENGINE = HERE.parent / "engine"
sys.dont_write_bytecode = True


def _base_checks() -> dict[str, Any]:
    return {
        "engine_dir": str(ENGINE),
        "ok": True,
        "errors": [],
        "warnings": [],
        "platform": sys.platform,
        "python_version": sys.version.split()[0],
    }


def _check_runtime(checks: dict[str, Any]) -> bool:
    if not sys.platform.startswith("linux"):
        checks["errors"].append(
            "Unsupported platform: use Linux Python. Real compilation requires "
            "a supported CANN toolchain (>=9.0.0)."
        )
        return False
    if sys.version_info < (3, 9):
        checks["errors"].append(
            f"Python {checks['python_version']} is too old; need >= 3.9"
        )
        return False
    return True


def _check_installation(checks: dict[str, Any]) -> bool:
    """Run the read-only package inventory before importing the engine."""
    sys.path.insert(0, str(HERE.parent))
    try:
        from face_selfcheck import inspect_face

        inventory = inspect_face(HERE.parents[1])
    except (ImportError, OSError) as exc:
        checks["errors"].append(f"package self-check unavailable: {exc}")
        return False
    checks["required_files_ok"] = inventory["ok"]
    checks["errors"].extend(inventory["errors"])
    return not checks["errors"]


def _check_engine(checks: dict[str, Any]) -> bool:
    sys.path.insert(0, str(HERE.parent))
    from face_selfcheck import read_skill_version

    checks["skill_version"] = read_skill_version(HERE.parents[1], checks["errors"])
    if checks["errors"]:
        return False
    if not (ENGINE / "harness/engine/__init__.py").is_file():
        checks["errors"].append(
            "bundled engine is missing; reinstall the complete package"
        )
        return False
    sys.path.insert(0, str(ENGINE))
    try:
        from harness.engine import common

        checks["project_root"] = str(common.PROJECT_ROOT)
        checks["project_root_is_engine"] = (
            Path(common.PROJECT_ROOT).resolve() == ENGINE.resolve()
        )
        if not checks["project_root_is_engine"]:
            checks["errors"].append(
                f"Imported engine belongs to {common.PROJECT_ROOT}, not this package; "
                "run preflight in a fresh Python process."
            )
    except Exception as exc:  # noqa: BLE001
        checks["errors"].append(f"engine import failed: {type(exc).__name__}: {exc}")
        return False

    try:
        signatures = common.load_api_signature_kb(ENGINE / "harness/knowledge")
        checks["api_signatures_entries"] = len(signatures)
        if not signatures:
            checks["errors"].append("API signature knowledge base contains no entries")
    except Exception as exc:  # noqa: BLE001
        checks["errors"].append(
            f"knowledge-base load failed: {type(exc).__name__}: {exc}"
        )

    try:
        import harness.engine.diagnose_compile_failure as diagnose_module

        checks["diagnose_run_bundle"] = callable(diagnose_module.run_bundle)
    except Exception as exc:  # noqa: BLE001
        checks["errors"].append(f"diagnosis import failed: {type(exc).__name__}: {exc}")
    return not checks["errors"]


def _check_toolchain(checks: dict[str, Any]) -> None:
    tools = {name: shutil.which(name) for name in ("ccec", "bisheng", "cmake")}
    checks["cann_tools"] = tools
    tracer = os.environ.get("ACD_STRACE") or shutil.which("strace")
    checks["compiler_trace"] = {
        "path": tracer,
        "available": bool(
            tracer and Path(tracer).is_file() and os.access(tracer, os.X_OK)
        ),
        "source": "ACD_STRACE" if os.environ.get("ACD_STRACE") else "PATH",
    }
    checks["compiler_receipts"] = {
        "primary": "temporary CMake observation hook and compile launcher",
        "strace": "optional fallback; availability does not prove ptrace permission",
        "confirmation": "execution receipt plus dependency, object, and target evidence",
    }
    checks["cann_available"] = bool(tools.get("ccec") or tools.get("bisheng"))
    if not checks["cann_available"]:
        checks["warnings"].append(
            "CANN compiler (ccec/bisheng) is not on PATH; diagnosis works, but "
            "real-compile verification needs Linux with a supported toolchain (>=9.0.0)."
        )
    checks["ascend_toolkit_home"] = os.environ.get("ASCEND_TOOLKIT_HOME")


def _check_host_search(checks: dict[str, Any]) -> None:
    """Observe host prerequisites without installing or downloading anything."""
    tools = {name: shutil.which(name) for name in ("opencode", "rg", "grep", "find")}
    checks["host_search"] = {
        "tools": tools,
        "harness_requires_ripgrep": False,
        "ripgrep_on_path": bool(tools["rg"]),
        "host_search_executed": False,
        "scope": "PATH discovery only; validate native host search separately",
    }
    if tools["opencode"] and not tools["rg"]:
        checks["warnings"].append(
            "OpenCode is on PATH but rg is not. Native host search or skill loading "
            "may attempt a download; provision a platform-matched ripgrep before "
            "an unattended run and validate host search. Harness diagnosis does "
            "not require ripgrep. No download was attempted by this check."
        )


def _check_version(checks: dict[str, Any]) -> None:
    try:
        from harness.engine import version_gate
        from harness.engine.env_probe import detect_cann

        checks["kb_cann_baseline"] = version_gate.KB_CANN_BASELINE
        detected = (detect_cann() or {}).get("version")
        checks["cann_version_detected"] = detected
        checks["version_compatibility"] = version_gate.classify(None, detected)
    except Exception as exc:  # noqa: BLE001
        checks["warnings"].append(
            f"version check unavailable: {type(exc).__name__}: {exc}"
        )


def run_checks() -> dict[str, Any]:
    checks = _base_checks()
    if not _check_runtime(checks):
        checks["ok"] = False
        return checks
    if not _check_installation(checks):
        checks["ok"] = False
        return checks
    try:
        __import__("yaml")
    except ImportError:
        checks["errors"].append(
            "PyYAML is missing. Create a user virtual environment, run "
            "python -m pip install PyYAML, and point ASCENDC_DOCTOR_PYTHON "
            "to that environment's Linux Python."
        )
        checks["ok"] = False
        return checks
    _check_engine(checks)
    _check_toolchain(checks)
    _check_host_search(checks)
    _check_version(checks)
    checks["ok"] = not checks["errors"]
    return checks


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--json", action="store_true", help="Print capability checks as JSON."
    )
    arguments = parser.parse_args(argv)
    checks = run_checks()
    exit_code = 0 if checks["ok"] else 1
    if not checks["ok"] and any("Unsupported platform" in e for e in checks["errors"]):
        exit_code = 2
    if arguments.json:
        print(json.dumps(checks, ensure_ascii=False, indent=2))
        return exit_code

    print(f"engine:         {checks['engine_dir']}")
    print(f"engine import:  {'OK' if checks.get('diagnose_run_bundle') else 'FAILED'}")
    print(f"PROJECT_ROOT:   {checks.get('project_root')}")
    print(f"API signatures: {checks.get('api_signatures_entries', '?')} entries")
    print(f"Python:         {checks.get('python_version')}")
    print(
        f"CANN compiler:  {'present' if checks.get('cann_available') else 'absent (diagnosis only)'}"
    )
    version = checks.get("version_compatibility") or {}
    print(
        f"Version gate:   baseline={version.get('kb_baseline', '?')} "
        f"detected={checks.get('cann_version_detected') or 'unknown'} "
        f"status={version.get('status', 'unavailable')}"
    )
    for warning in checks["warnings"]:
        print(f"  [warn] {warning}")
    for error in checks["errors"]:
        print(f"  [error] {error}")
    print(f"\nstatus: {'OK' if checks['ok'] else 'NOT USABLE'}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
