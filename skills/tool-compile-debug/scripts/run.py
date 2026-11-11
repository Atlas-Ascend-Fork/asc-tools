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
"""Run the command-line entry points shipped with this package.

用途：包内日志诊断、按需取证、验证/观察及脱敏入口的统一启动器。
使用方法：
    python3 run.py diagnose <工程目录> <构建日志> <输出目录> [SoC] [--json-stdout]
    python3 run.py verify <工程目录> <输出JSON> [--previous-result <前次证据JSON>]
    python3 run.py inspect --project <工程目录> --file <源码> --out <输出JSON>
    python3 run.py verify --observe-run <执行目录>
    python3 run.py redact <输入文件> <输出文件> [--stats <统计JSON>]
The bundled engine is loaded directly from ``scripts/engine``. This keeps the
package self-contained while preserving the canonical diagnose, verify, and
redact interfaces.

Usage examples::

    python run.py diagnose --project CASE --build-log LOG --out OUT --soc Ascend910B
    python run.py verify CASE OUT.json [--apply-patch PATCH] [--dry-run]
    python run.py redact INPUT OUTPUT [--custom-words WORD ...]

All runtime output must live outside the installed package. Real-compile
verification additionally requires Linux and a supported CANN toolchain.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
from collections.abc import Callable
from contextlib import ExitStack
from pathlib import Path

HERE = Path(__file__).resolve()
ENGINE = HERE.parent / "engine"
# Keep summaries bounded while retaining the complete error in the saved report.
MAX_SUMMARY_ERROR_CHARS = 500
sys.dont_write_bytecode = True


def _force_utf8_io() -> None:
    """Keep structured output lossless on consoles with a non-UTF-8 codepage."""
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is None:
            continue
        try:
            reconfigure(encoding="utf-8")
        except (OSError, ValueError):
            # Captured or detached streams may not support reconfiguration.
            pass


def _ensure_engine_on_path() -> None:
    """Load imports from this package's engine before any system installation."""
    engine_path = str(ENGINE)
    if not (ENGINE / "harness/engine/__init__.py").is_file():
        sys.stderr.write(
            f"bundled engine not found at {ENGINE}\n"
            "Reinstall the complete package; scripts/engine is part of the package.\n"
        )
        raise SystemExit(2)
    loaded = sys.modules.get("harness.engine")
    loaded_path = getattr(loaded, "__file__", None)
    if loaded is not None and (
        loaded_path is None
        or Path(loaded_path).resolve().parent != ENGINE.resolve() / "harness/engine"
    ):
        raise SystemExit(
            "A different skills package is already imported; run this entry point "
            "in a fresh Python process."
        )
    if engine_path in sys.path:
        sys.path.remove(engine_path)
    sys.path.insert(0, engine_path)


USAGE = (
    "usage: run.py {diagnose|inspect|memory|verify|redact} ...\n"
    "  diagnose  PROJECT BUILD_LOG OUT [SOC] [--run-probes] [--verify] [--json-stdout]\n"
    "  verify    CASE JSON_OUT [--apply-patch F | --patch-suggestions J] [--soc S]\n"
    "            [--previous-result P] [--build-arg=ARG ... | --target T --cmake-arg=ARG ...]\n"
    "            [--dry-run] [--sandbox]\n"
    "  verify    --summary RESULT.json  (read an existing report; no build)\n"
    "  inspect   --project DIR --question source|build|environment --out JSON [--file FILE]\n"
    "  verify    --observe-run RUN_DIR (read durable execution receipt; no build)\n"
    "  memory    --project DIR [--state JSON] [--event JSON] --out NEW_JSON\n"
    "  verify    --recover-verification DIR --json-out NEW_JSON (collect; no build)\n"
    "  redact    IN OUT [--custom-words ...] [--stats S] [--reversible]\n"
    "\n"
    "Positional arguments map to engine flags; additional flags pass through.\n"
    "The engine flag form (--project/--build-log/--out, etc.) is also accepted.\n"
)


def _split_pos_flags(arguments: list[str]) -> tuple[list[str], list[str]]:
    """Split positional arguments from flags at the first option token."""
    for index, token in enumerate(arguments):
        if token.startswith("-"):
            return arguments[:index], arguments[index:]
    return arguments, []


def _diagnose_argv(arguments: list[str]) -> list[str]:
    if arguments and arguments[0].startswith("-"):
        return arguments
    positional, flags = _split_pos_flags(arguments)
    if len(positional) < 3:
        raise SystemExit("diagnose needs: PROJECT BUILD_LOG OUT [SOC]")
    if len(positional) > 4:
        raise SystemExit(
            "diagnose: unexpected positional argument(s): " + " ".join(positional[4:])
        )
    soc = ["--soc", positional[3]] if len(positional) == 4 else []
    return [
        "--project",
        positional[0],
        "--build-log",
        positional[1],
        "--out",
        positional[2],
        *soc,
        *flags,
    ]


def _verify_argv(arguments: list[str]) -> list[str]:
    arguments = _normalize_cmake_arguments(arguments)
    if not arguments:
        raise SystemExit(
            "verify needs: CASE JSON_OUT or --case CASE --json-out JSON_OUT"
        )
    positional, flags = _split_pos_flags(arguments)
    if len(positional) > 2:
        raise SystemExit(
            "verify: unexpected positional argument(s): " + " ".join(positional[2:])
        )
    for name, position in (("--case", 0), ("--json-out", 1)):
        occurrences = sum(
            token.startswith("--")
            and len(token.partition("=")[0]) > 2
            and name.startswith(token.partition("=")[0])
            for token in flags
        )
        if occurrences > 1 or (occurrences and len(positional) > position):
            raise SystemExit(
                f"verify: specify {name} only once; use CASE JSON_OUT, "
                "CASE --json-out JSON_OUT, or --case CASE --json-out JSON_OUT."
            )
    if not positional:
        return arguments
    if len(positional) == 1:
        if _path_option(flags, "--json-out") is None:
            raise SystemExit(
                "verify needs an output path: CASE JSON_OUT or "
                "CASE --json-out JSON_OUT."
            )
        return ["--case", positional[0], *flags]
    return ["--case", positional[0], "--json-out", positional[1], *flags]


def _normalize_cmake_arguments(arguments: list[str]) -> list[str]:
    """Preserve each shell token while accepting a separated CMake -D value."""
    normalized: list[str] = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument.partition("=")[0] == "--cmake-args":
            raise SystemExit(
                "verify: --cmake-args is not supported. Repeat --cmake-arg once "
                "per CMake argument, for example "
                "--cmake-arg=-DOPTION=VALUE --cmake-arg=-DCMAKE_BUILD_TYPE=Release. "
                "Do not combine multiple arguments into one quoted string."
            )
        if (
            argument == "--cmake-arg"
            and index + 1 < len(arguments)
            and arguments[index + 1].startswith("-D")
        ):
            normalized.append(f"--cmake-arg={arguments[index + 1]}")
            index += 2
            continue
        normalized.append(argument)
        index += 1
    return normalized


def _redact_argv(arguments: list[str]) -> list[str]:
    if arguments and arguments[0].startswith("-"):
        return arguments
    positional, flags = _split_pos_flags(arguments)
    if len(positional) < 2:
        raise SystemExit("redact needs: IN OUT")
    if len(positional) > 2:
        raise SystemExit(
            "redact: unexpected positional argument(s): " + " ".join(positional[2:])
        )
    return ["--in", positional[0], "--out", positional[1], *flags]


def _guard_pack_purity(arguments: list[str], command: str | None = None) -> None:
    """Reject output paths that would write into the installed package."""
    package_root = ENGINE.parents[1].resolve()
    output_options = ("--out", "--json-out", "--stats", "--meta-out", "--recovery-dir")
    outputs: list[Path] = []
    for index, argument in enumerate(arguments):
        option, separator, value = argument.partition("=")
        is_output = option == "-o" or (
            option.startswith("--")
            and len(option) > 2
            and any(known.startswith(option) for known in output_options)
        )
        if not is_output:
            continue
        if separator:
            outputs.append(Path(value))
        elif index + 1 < len(arguments):
            outputs.append(Path(arguments[index + 1]))

    if command == "verify" or (command == "diagnose" and "--verify" in arguments):
        workspace_root = os.environ.get("ACD_WORKSPACE_ROOT") or os.environ.get(
            "ACD_SCRATCH_ROOT"
        )
        outputs.append(
            Path(workspace_root)
            if workspace_root
            else Path(tempfile.gettempdir()) / "acd_verify_work"
        )

    package_outputs = [
        str(path)
        for path in outputs
        if package_root in path.resolve().parents or path.resolve() == package_root
    ]
    if package_outputs:
        sys.stderr.write(
            "[guard] output path is inside the installed package; refusing to write\n"
            f"package={package_root} offending={package_outputs}\n"
            "Write reports and build scratch outside the installed package.\n"
        )
        raise SystemExit(2)


def _path_option(arguments: list[str], name: str) -> Path | None:
    """Read the last effective path, including unambiguous option abbreviations."""
    selected: Path | None = None
    for index, argument in enumerate(arguments):
        option, separator, value = argument.partition("=")
        if not (
            option.startswith("--") and len(option) > 2 and name.startswith(option)
        ):
            continue
        if separator:
            selected = Path(value)
        elif index + 1 < len(arguments):
            selected = Path(arguments[index + 1])
    return selected


def _audit_arguments(command: str, arguments: list[str]) -> dict[str, object]:
    source_names = {
        "verify": "--case",
        "diagnose": "--project",
        "inspect": "--project",
        "memory": "--project",
        "redact": "--in",
    }
    source = _path_option(arguments, source_names[command])
    output = _path_option(arguments, "--json-out" if command == "verify" else "--out")
    forbidden_roots = [ENGINE.parents[1].resolve()]
    if source is not None:
        forbidden_roots.append(
            (source.parent if command == "redact" else source).resolve()
        )
    output_dir = output if command == "diagnose" else output.parent if output else None
    return {"output_dir": output_dir, "forbidden_roots": forbidden_roots}


def _dispatch(command: str, arguments: list[str]) -> int:
    """Invoke the selected engine module after normalizing ``sys.argv``."""
    if command == "diagnose":
        from harness.engine.diagnose_compile_failure import main as engine_main

        module_name = "harness.engine.diagnose_compile_failure"
    elif command == "verify":
        from harness.engine.verify_runner import main as engine_main

        module_name = "harness.engine.verify_runner"
    elif command == "redact":
        from harness.engine.redactor import main as engine_main

        module_name = "harness.engine.redactor"
    elif command == "inspect":
        from harness.engine.inspect_context import main as engine_main

        module_name = "harness.engine.inspect_context"
    elif command == "memory":
        from harness.engine.repair_memory import main as engine_main

        module_name = "harness.engine.repair_memory"
    else:
        raise ValueError(f"unsupported command: {command}")
    sys.argv = [module_name, *arguments]
    return engine_main()


def _report_object(data: dict, name: str) -> dict:
    """Accept absent legacy fields, but do not reinterpret malformed objects."""
    value = data.get(name)
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise TypeError(f"report field {name} must be an object or null")
    return value


def _report_field(data: dict, name: str, kind: type) -> object:
    """Keep missing fields unknown and reject an incompatible saved schema."""
    value = data.get(name)
    if value is not None and (
        not isinstance(value, kind) or (kind is int and isinstance(value, bool))
    ):
        raise TypeError(f"report field {name} must be {kind.__name__} or null")
    return value


def _text_fields(data: dict, names: tuple[str, ...]) -> dict[str, object]:
    return {name: _report_field(data, name, str) for name in names}


def _report_names(data: dict, name: str) -> str | list[str] | None:
    """Targets and architectures may be singular or plural; never expand objects."""
    value = data.get(name)
    if value is None or isinstance(value, str):
        return value
    if isinstance(value, list) and all(isinstance(item, str) for item in value):
        return value
    raise TypeError(f"report field {name} must be a string, string list or null")


def _verify_summary(result_path: Path) -> dict[str, object]:
    """Project report facts without expanding receipts or deriving a verdict."""
    data = json.loads(result_path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not isinstance(data.get("transition"), str):
        raise TypeError("expected a verify report object with a transition string")
    after = _report_object(data, "after")
    logs = _report_object(data, "logs")
    evidence = _report_object(data, "compilation_evidence")
    target = _report_object(data, "target_validation")
    coverage = _report_object(target, "prior_failure_coverage")
    contract = _report_object(data, "contract_check")
    suspect = _report_object(data, "escape_suspect")
    scope = _report_object(data, "validation_scope")
    error = _report_object(data, "error")
    coverage_summary = _text_fields(coverage, ("status", "reason"))
    for name in (
        "required_files",
        "missing_files",
        "unresolved_files",
        "reanchored_files",
    ):
        value = _report_field(coverage, name, list)
        coverage_summary[name + "_count"] = len(value) if value is not None else None
    error_message = _report_field(error, "message", str)
    missing_symbols = _report_field(suspect, "missing_symbols", list)
    suspect_summary = None
    if data.get("escape_suspect") is not None:
        suspect_summary = {
            **_text_fields(suspect, ("check", "evidence_log")),
            "missing_symbols_count": (
                len(missing_symbols) if missing_symbols is not None else None
            ),
        }
    return {
        "schema_version": "verify-summary-v1",
        "result_path": str(result_path.resolve()),
        "transition": data.get("transition"),
        "reason": _report_field(data, "reason", str),
        "build_returncode": _report_field(after, "returncode", int),
        "build_invoked": _report_field(data, "build_invoked", bool),
        "dry_run": _report_field(data, "dry_run", bool),
        "built_target": _report_names(data, "built_target"),
        "expected_target": _report_names(data, "expected_target"),
        "built_arch": _report_names(data, "built_arch"),
        "compilation_evidence": _report_field(evidence, "status", str),
        "compilation_reason": _report_field(evidence, "reason", str),
        "target_validation": {
            **_text_fields(target, ("status", "requested_arch")),
            "observed_architectures": _report_names(target, "observed_architectures"),
        },
        "prior_failure_coverage": coverage_summary,
        "contract_check": _text_fields(contract, ("status", "reason")),
        "escape_suspect": suspect_summary,
        "validation_scope": _text_fields(scope, ("compile", "contract", "numerical")),
        "error": {
            **_text_fields(error, ("type", "stage")),
            "message": error_message[:MAX_SUMMARY_ERROR_CHARS]
            if error_message is not None
            else None,
            "message_truncated": bool(
                error_message and len(error_message) > MAX_SUMMARY_ERROR_CHARS
            ),
        },
        "log_path": _report_field(after, "log_path", str)
        or _report_field(logs, "after", str),
        "recipe_cwd": _report_field(after, "recipe_cwd", str),
    }


def _read_verify_summary(arguments: list[str]) -> int:
    """Read an explicit saved report without importing or invoking the engine."""
    if len(arguments) == 2 and arguments[0] == "--summary":
        filename = arguments[1]
    elif len(arguments) == 1 and arguments[0].startswith("--summary="):
        filename = arguments[0].partition("=")[2]
    else:
        sys.stderr.write(
            "usage: run.py verify --summary RESULT.json (no build options)\n"
        )
        return 64
    if not filename or filename.startswith("--"):
        sys.stderr.write("verify --summary requires an existing result JSON path.\n")
        return 64
    try:
        summary = _verify_summary(Path(filename))
    except (OSError, ValueError, TypeError) as exc:
        sys.stderr.write(
            f"[verify] cannot read summary ({type(exc).__name__}): {exc}\n"
        )
        return 2
    sys.stdout.write(json.dumps(summary, ensure_ascii=False) + "\n")
    return 0


def _print_verify_summary(arguments: list[str]) -> None:
    """Expose actual report facts without changing stdout or the build verdict."""
    result_path = _path_option(arguments, "--json-out")
    if result_path is None:
        return
    try:
        summary = _verify_summary(result_path)
    except (OSError, ValueError, TypeError):
        sys.stderr.write("[verify] summary unavailable; inspect the result JSON.\n")
        return
    sys.stderr.write("[verify] " + json.dumps(summary, ensure_ascii=False) + "\n")


def main(argv: list[str] | None = None) -> int:
    _force_utf8_io()
    arguments = list(sys.argv[1:] if argv is None else argv)
    if not arguments or arguments[0] in {"-h", "--help"}:
        sys.stdout.write(USAGE)
        return 0 if arguments else 64

    command, rest = arguments[0], arguments[1:]
    adapters: dict[str, Callable[[list[str]], list[str]]] = {
        "diagnose": _diagnose_argv,
        "verify": _verify_argv,
        "redact": _redact_argv,
        "inspect": lambda arguments: arguments,
        "memory": lambda arguments: arguments,
    }
    if command not in adapters:
        sys.stderr.write(f"unknown subcommand: {command}\n{USAGE}")
        return 64
    if command == "verify" and any(
        token.partition("=")[0] == "--summary" for token in rest
    ):
        return _read_verify_summary(rest)
    if command == "verify" and any(x.split("=", 1)[0] == "--observe-run" for x in rest):
        _ensure_engine_on_path()
        return _dispatch(command, rest)
    if command == "verify" and any(
        x.split("=", 1)[0] == "--recover-verification" for x in rest
    ):
        _guard_pack_purity(rest, command)
        _ensure_engine_on_path()
        result = _dispatch(command, rest)
        if result == 0:
            _print_verify_summary(rest)
        return result
    if rest in (["-h"], ["--help"]):
        if command == "verify":
            sys.stdout.write(
                "Saved report only: run.py verify --summary RESULT.json (no build).\n"
            )
        _ensure_engine_on_path()
        return _dispatch(command, rest)
    try:
        normalized = adapters[command](rest)
    except SystemExit as exc:
        message = str(exc)
        if message:
            sys.stderr.write(f"{message}\n")
        return 64

    _guard_pack_purity(normalized, command)
    if not sys.platform.startswith("linux"):
        sys.stderr.write(
            "Unsupported platform: this package runs on Linux. Use a Linux Python "
            "environment with a supported CANN toolchain for compilation.\n"
        )
        return 2

    _ensure_engine_on_path()
    from harness.engine.audit import audit_location

    with ExitStack() as stack:
        try:
            stack.enter_context(audit_location(**_audit_arguments(command, normalized)))
        except (OSError, RuntimeError, ValueError) as exc:
            sys.stderr.write(
                f"[guard] audit_location_rejected ({type(exc).__name__}): {exc}\n"
            )
            return 2
        result = _dispatch(command, normalized)
        if command == "verify" and result == 0:
            _print_verify_summary(normalized)
        return result


if __name__ == "__main__":
    raise SystemExit(main())
