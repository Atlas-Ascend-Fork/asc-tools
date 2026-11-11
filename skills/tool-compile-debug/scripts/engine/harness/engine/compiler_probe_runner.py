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
import os
import re
import shlex
import shutil
import tempfile
import time
from pathlib import Path
from typing import Any

from harness.engine.probe_msopgen_regenerate_poc import (
    run_poc as run_msopgen_regenerate_poc,
)

from harness.engine.common import PROJECT_ROOT
from harness.engine.common import error_payload
from harness.engine.common import load_json
from harness.engine.common import run_command
from harness.engine.common import which
from harness.engine.common import write_json

DEFAULT_PROBE_TIMEOUT_S = 120.0


def _status(returncode: int) -> str:
    if returncode == 0:
        return "passed"
    if returncode == 124:
        return "timeout"
    if returncode == 126:
        return "permission_error"
    if returncode == 127:
        return "tool_missing"
    return "failed"


def _write_log(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", errors="replace")


def _tool_family_from_command(command: str | list[str] | None) -> str:
    if isinstance(command, list):
        text = " ".join(command)
    else:
        text = command or ""
    lower = text.lower()
    if "bisheng" in lower:
        return "bisheng"
    if "ccec" in lower:
        return "ccec"
    return "unknown_compiler"


def _diagnostic_weight(status: str, evidence_kind: str) -> str:
    if status not in {"passed", "failed"}:
        return "none"
    if evidence_kind in {
        "direct_kernel_compile",
        "preprocess",
        "optimization_sensitivity",
    }:
        return "strong"
    if evidence_kind in {"environment", "artifact_metadata", "regenerate_diff"}:
        return "supporting"
    return "weak"


def _probe_metadata(
    *,
    name: str,
    status: str,
    command: str | list[str] | None,
    evidence_kind: str,
    tool_family: str | None = None,
    invocation: str = "on_demand",
) -> dict[str, Any]:
    return {
        "tool_family": tool_family or _tool_family_from_command(command),
        "evidence_kind": evidence_kind,
        "diagnostic_weight": _diagnostic_weight(status, evidence_kind),
        "invocation": invocation,
    }


def _skipped_probe(
    name: str,
    *,
    reason: str,
    evidence_kind: str,
    tool_family: str = "unknown_compiler",
    invocation: str = "on_demand",
    timeout_s: float | None = None,
) -> dict[str, Any]:
    status = "skipped"
    return {
        "name": name,
        "command": "",
        "status": status,
        "returncode": None,
        "log_path": None,
        "reason": reason,
        "elapsed_s": 0.0,
        "timeout_s": float(timeout_s) if timeout_s is not None else None,
        **_probe_metadata(
            name=name,
            status=status,
            command="",
            evidence_kind=evidence_kind,
            tool_family=tool_family,
            invocation=invocation,
        ),
    }


def _run_probe(
    name: str,
    command: list[str],
    cwd: Path | None,
    log_dir: Path,
    timeout: float = DEFAULT_PROBE_TIMEOUT_S,
    *,
    evidence_kind: str = "environment",
    tool_family: str | None = None,
    invocation: str = "on_demand",
) -> dict[str, Any]:
    started = time.perf_counter()
    res = run_command(command, cwd=cwd, timeout=timeout)
    elapsed_s = round(time.perf_counter() - started, 3)
    log_path = log_dir / f"{name}.log"
    _write_log(
        log_path,
        "$ "
        + " ".join(shlex.quote(x) for x in command)
        + "\n"
        + (res.get("output") or ""),
    )
    status = _status(res["returncode"])
    return {
        "name": name,
        "command": " ".join(shlex.quote(x) for x in command),
        "status": status,
        "returncode": res["returncode"],
        "log_path": str(log_path),
        "elapsed_s": elapsed_s,
        "timeout_s": float(timeout),
        **_probe_metadata(
            name=name,
            status=status,
            command=command,
            evidence_kind=evidence_kind,
            tool_family=tool_family,
            invocation=invocation,
        ),
    }


def _probe_status_from_msopgen_conclusion(status: str) -> str:
    if status.startswith("regenerated_"):
        return "passed"
    if status in {"blocked_tool_missing", "blocked_input_missing"}:
        return "skipped"
    return "failed"


def _new_scratch(case_dir: Path, prefix: str, parent: Path | None = None) -> Path:
    """Create an owned run directory, never beneath the source or installation."""
    case_dir = case_dir.resolve()
    root = (
        parent or Path(os.environ.get("ACD_SCRATCH_ROOT") or tempfile.gettempdir())
    ).resolve()
    forbidden = [case_dir, PROJECT_ROOT.resolve()]
    if (PROJECT_ROOT.parent / "SKILL.md").is_file():
        forbidden.append(PROJECT_ROOT.parent.resolve())
    if any(root == p or p in root.parents for p in forbidden):
        raise ValueError(
            "scratch must be outside the source tree and installed package; set ACD_SCRATCH_ROOT"
        )
    root.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix=prefix, dir=root))


def _copy_isolated_source(case_dir: Path, destination: Path) -> Path:
    """Copy source bytes without retaining writable aliases into the original."""
    case_dir = case_dir.resolve()
    if not case_dir.is_dir():
        raise ValueError(f"source directory does not exist: {case_dir}")
    destination = destination.resolve()
    if destination == case_dir or case_dir in destination.parents:
        raise ValueError("isolated copy overlaps source tree")
    for directory, dirs, files in os.walk(case_dir, followlinks=False):
        dirs[:] = [name for name in dirs if name not in {".git", "__pycache__"}]
        for name in [*dirs, *files]:
            path = Path(directory) / name
            if path.is_symlink() or getattr(path, "is_junction", lambda: False)():
                raise ValueError(
                    f"source symlink requires an explicitly materialized dependency: {path}"
                )
    shutil.copytree(
        case_dir,
        destination,
        copy_function=shutil.copyfile,
        ignore=shutil.ignore_patterns(".git", "__pycache__"),
    )
    # A read-only source installation must still yield a writable private build.
    destination.chmod(destination.stat().st_mode | 0o700)
    for path in destination.rglob("*"):
        if path.is_dir():
            path.chmod(path.stat().st_mode | 0o700)
    return destination


def _command_source(entry: dict[str, Any], case_dir: Path) -> Path:
    directory = Path(str(entry.get("directory") or case_dir))
    if not directory.is_absolute():
        directory = case_dir / directory
    source = Path(str(entry.get("file") or ""))
    return (source if source.is_absolute() else directory / source).resolve()


def _first_compile_command(
    context: dict[str, Any], want_kernel: bool = True, case_dir: Path | None = None
) -> dict[str, Any] | None:
    """Choose the failed TU; never silently choose an unrelated first entry."""
    case_dir = (case_dir or Path(context.get("case_dir") or ".")).resolve()
    build = context.get("build") or {}
    cmds = build.get("compile_commands") or build.get("compile_commands_excerpt") or []
    cmds = [entry for entry in cmds if isinstance(entry, dict) and entry.get("file")]
    failed = (context.get("primary_error") or {}).get("file")
    if failed:
        failed_path = Path(str(failed))
        resolved = (
            failed_path if failed_path.is_absolute() else case_dir / failed_path
        ).resolve()
        exact = [
            entry for entry in cmds if _command_source(entry, case_dir) == resolved
        ]
        if len(exact) == 1:
            return exact[0]
        if len(exact) > 1:
            return None
        # A basename in a compiler message is usable only when unambiguous.
        if failed_path.name == str(failed_path):
            by_name = [
                entry
                for entry in cmds
                if Path(str(entry["file"])).name == failed_path.name
            ]
            return by_name[0] if len(by_name) == 1 else None
        return None
    if len(cmds) == 1:
        return cmds[0]
    return None


def _captured_argv(entry: dict[str, Any]) -> list[str]:
    arguments = entry.get("arguments")
    if arguments is not None:
        if (
            not isinstance(arguments, list)
            or not arguments
            or not all(isinstance(a, str) for a in arguments)
        ):
            raise ValueError(
                "compile_commands arguments must be a nonempty string array"
            )
        argv = list(arguments)
    else:
        command = str(entry.get("command") or "")
        if re.search(r"[;&|\n\r\x60$<>]", command):
            raise ValueError(
                "captured shell syntax is not a direct compiler invocation"
            )
        argv = shlex.split(command)
    if not argv:
        raise ValueError("captured compiler argv is empty")
    compiler = Path(argv[0]).name.lower()
    if not re.fullmatch(
        r"(?:bisheng|ccec|clang(?:\+\+)?|gcc|g\+\+|cc|c\+\+)(?:-[0-9.]+)?(?:\.exe)?",
        compiler,
    ):
        raise ValueError(
            "captured command must invoke a compiler directly, without a shell or launcher"
        )
    if not any(flag in argv for flag in ("-c", "-E", "-fsyntax-only")):
        raise ValueError("captured command is not a compile-only invocation")
    for arg in argv[1:]:
        if arg.startswith(
            (
                "@",
                "-X",
                "-Wp,",
                "-Wa,",
                "-Wl,",
                "-B",
                "-fplugin",
                "-fpass-plugin",
                "-specs",
                "--specs",
                "--config",
                "-load",
            )
        ):
            raise ValueError(f"unsupported indirect compiler argument: {arg}")
    return argv


def _prepare_compile_probe(
    entry: dict[str, Any],
    case_dir: Path,
    work: Path,
    *,
    opt: str | None = None,
    preprocess: bool = False,
) -> tuple[list[str], Path]:
    """Retain captured flags while redirecting every supported output to work."""
    case_dir = case_dir.resolve()
    argv = _captured_argv(entry)
    directory = Path(str(entry.get("directory") or case_dir))
    directory = (
        directory if directory.is_absolute() else case_dir / directory
    ).resolve()
    try:
        relative_cwd = directory.relative_to(case_dir)
        _command_source(entry, case_dir).relative_to(case_dir)
    except ValueError as exc:
        raise ValueError(
            "captured cwd/source is outside the provided source tree"
        ) from exc
    expected_source = _command_source(entry, case_dir)
    if not expected_source.is_file():
        raise ValueError("captured translation unit is missing")
    value_flags = {
        "-o",
        "-MF",
        "-MJ",
        "-MT",
        "-MQ",
        "-I",
        "-isystem",
        "-iquote",
        "-include",
        "-imacros",
        "-idirafter",
        "-x",
        "-target",
        "--target",
        "-isysroot",
        "--sysroot",
        "-resource-dir",
        "-D",
        "-U",
        "-arch",
    }
    operands = []
    skip = False
    for token in argv[1:]:
        if skip:
            skip = False
            continue
        if token in value_flags:
            skip = True
            continue
        if token.startswith("-"):
            continue
        candidate = Path(token)
        resolved = (
            candidate if candidate.is_absolute() else directory / candidate
        ).resolve()
        if (
            candidate.suffix.lower() in {".c", ".cc", ".cpp", ".cxx", ".asc", ".s"}
            or resolved == expected_source
        ):
            operands.append(resolved)
    if operands != [expected_source]:
        raise ValueError(
            "captured argv does not compile exactly the selected translation unit"
        )
    clone = _copy_isolated_source(case_dir, work / "source")
    cwd = clone / relative_cwd
    if not cwd.is_dir():
        raise ValueError("captured compiler cwd is missing from source copy")
    outputs = work / "outputs"
    outputs.mkdir()
    # Split and joined forms accepted by GCC/Clang/CANN. Other compiler-mediated
    # writers are rejected below rather than silently escaping the owned folder.
    path_options = (
        "-o",
        "-MF",
        "-MJ",
        "--output",
        "-dependency-file",
        "-serialize-diagnostics",
        "--serialize-diagnostics",
        "-aux-info",
    )
    output_index = 0
    rewritten = [argv[0]]
    i = 1
    while i < len(argv):
        arg = argv[i]
        matched = next(
            (
                flag
                for flag in path_options
                if arg == flag
                or arg.startswith(flag + "=")
                or (
                    flag in {"-o", "-MF", "-MJ"}
                    and arg.startswith(flag)
                    and len(arg) > len(flag)
                )
            ),
            None,
        )
        if matched:
            if arg == matched:
                if i + 1 >= len(argv):
                    raise ValueError(f"missing output value for {matched}")
                value = argv[i + 1]
                i += 1
            else:
                value = arg[len(matched) :].lstrip("=")
            output_index += 1
            filename = Path(value).name or "output"
            rewritten.extend([matched, str(outputs / f"{output_index}-{filename}")])
        elif arg.startswith(
            (
                "-fprofile",
                "-fmodule-output",
                "-fmodules-cache-path",
                "-fdiagnostics-serialization",
                "-foptimization-record",
                "-ftime-trace",
                "-dumpdir",
                "-dumpbase",
                "--save-temps",
                "-fopt-info",
                "-fdiagnostics-file",
                "-gen-cdb-fragment-path",
                "-coverage-notes-file",
                "-coverage-data-file",
            )
        ):
            raise ValueError(f"unsupported compiler output option: {arg}")
        elif opt and re.fullmatch(r"-O(?:[0-3sz]|fast|g)", arg):
            pass
        elif preprocess and arg in {"-c", "-E", "-S"}:
            pass
        else:
            # Rebase source/include paths owned by the project, including joined
            # -I/path and --sysroot=/path; external toolchain headers stay read-only.
            original = str(case_dir)
            rebased = arg.replace(original + os.sep, str(clone) + os.sep)
            rebased = rebased.replace(case_dir.as_posix() + "/", clone.as_posix() + "/")
            if rebased == original:
                rebased = str(clone)
            rewritten.append(rebased)
        i += 1
    if opt:
        rewritten.append(opt)
    if preprocess:
        rewritten.append("-E")
    if "-fmodules" in argv or "-fcxx-modules" in argv:
        rewritten.append("-fmodules-cache-path=" + str(outputs / "module-cache"))
    # A last explicit output overrides any earlier -o and prevents implicit cwd
    # output even when the captured invocation omitted -o.
    rewritten.extend(
        ["-o", str(outputs / ("preprocessed.i" if preprocess else "probe.o"))]
    )
    return rewritten, cwd


def _write_reduction_scaffold(
    case_dir: Path, context: dict[str, Any], reason: str
) -> dict[str, Any]:
    out_dir = _new_scratch(case_dir, "acd-reduction-")
    compile_cmd = _first_compile_command(context, case_dir=case_dir)
    if not compile_cmd:
        write_json(
            out_dir / "environment.json",
            {"reason": "failed translation unit is not uniquely identified"},
        )
        return {
            "reduction_dir": str(out_dir),
            "reason": "failed translation unit is not uniquely identified",
        }
    try:
        command, cwd = _prepare_compile_probe(compile_cmd, case_dir, out_dir)
    except ValueError as exc:
        write_json(out_dir / "environment.json", {"reason": str(exc)})
        return {"reduction_dir": str(out_dir), "reason": str(exc)}
    compile_sh = out_dir / "compile.sh"
    compile_sh.write_text(
        "#!/usr/bin/env bash\nset -euo pipefail\ncd "
        + shlex.quote(str(cwd))
        + "\nexec "
        + shlex.join(command)
        + "\n",
        encoding="utf-8",
    )
    compile_sh.chmod(0o755)
    write_json(
        out_dir / "environment.json",
        {"argv": command, "cwd": str(cwd), "source": compile_cmd["file"]},
    )
    (out_dir / "error.log").write_text(reason + "\n", encoding="utf-8")
    (out_dir / "reduction_report.md").write_text(
        "# Compiler Reduction Scaffold\n\nPrivate source copy and captured flags; not yet minimized or verified.\n",
        encoding="utf-8",
    )
    return {"reduction_dir": str(out_dir), "compile_sh": str(compile_sh)}


def _probe_status_map(probes: list[dict[str, Any]]) -> dict[str, str]:
    return {str(p.get("name")): str(p.get("status")) for p in probes if p.get("name")}


def _probe_evidence(probes: list[dict[str, Any]]) -> list[str]:
    return [
        f"{p.get('name')}={p.get('status')}"
        for p in probes
        if p.get("name") and p.get("status")
    ]


def _log_contains(probe: dict[str, Any], needle: str) -> bool:
    log_path = probe.get("log_path")
    if not log_path:
        return False
    path = Path(str(log_path))
    if not path.exists():
        return False
    return needle.lower() in path.read_text(encoding="utf-8", errors="replace").lower()


def _interpret_probes(
    case_dir: Path, context: dict[str, Any], probes: list[dict[str, Any]]
) -> dict[str, Any]:
    evidence = _probe_evidence(probes)
    if not probes or all(p.get("status") == "skipped" for p in probes):
        return {"likely": "unknown", "confidence": 0.35, "evidence": evidence}

    if any(_log_contains(p, "internal compiler error") for p in probes):
        scaffold = _write_reduction_scaffold(
            case_dir, context, "internal compiler error detected in probe output"
        )
        return {
            "likely": "compiler_internal",
            "confidence": 0.82,
            "evidence": ["internal compiler error detected", scaffold["reduction_dir"]],
        }

    msopgen_probe = next(
        (p for p in probes if p.get("name") == "probe_msopgen_regenerate"), None
    )
    if (
        msopgen_probe
        and msopgen_probe.get("conclusion_status") == "regenerated_with_diffs"
    ):
        return {
            "likely": "msopgen_regenerate_diff",
            "confidence": 0.72,
            "evidence": [
                f"changed_count={msopgen_probe.get('changed_count', 0)}",
                str(msopgen_probe.get("result_md") or ""),
            ],
        }

    compiler_probes = [
        p
        for p in probes
        if p.get("evidence_kind")
        in {"direct_kernel_compile", "preprocess", "optimization_sensitivity"}
    ]
    if not compiler_probes or all(
        p.get("status") == "skipped" for p in compiler_probes
    ):
        return {"likely": "unknown", "confidence": 0.35, "evidence": evidence}

    statuses = _probe_status_map(compiler_probes)
    if statuses.get("probe_O0") == "passed" and (
        statuses.get("probe_O2") == "failed" or statuses.get("probe_O3") == "failed"
    ):
        return {
            "likely": "optimization_sensitive_or_compiler_limit",
            "confidence": 0.74,
            "evidence": evidence,
        }

    preprocess_ok = statuses.get("probe_preprocess_only") == "passed"
    compile_probe_statuses = [
        status
        for name, status in statuses.items()
        if name in {"probe_direct_kernel_compile", "probe_O0", "probe_O2", "probe_O3"}
    ]
    if (
        preprocess_ok
        and compile_probe_statuses
        and all(status == "failed" for status in compile_probe_statuses)
    ):
        return {
            "likely": "compiler_backend_after_preprocess",
            "confidence": 0.68,
            "evidence": evidence,
        }

    if any(p.get("status") == "tool_missing" for p in compiler_probes):
        return {
            "likely": "toolchain_probe_unavailable",
            "confidence": 0.30,
            "evidence": evidence,
        }

    failed = [p for p in compiler_probes if p.get("status") == "failed"]
    passed = [p for p in compiler_probes if p.get("status") == "passed"]
    if failed and passed:
        return {
            "likely": "phase_sensitive_or_command_specific_failure",
            "confidence": 0.62,
            "evidence": evidence,
        }
    if failed:
        return {
            "likely": "reproducible_compile_failure",
            "confidence": 0.58,
            "evidence": evidence,
        }
    return {"likely": "unknown", "confidence": 0.35, "evidence": evidence}


def run_probes(
    case_dir: Path,
    context: dict[str, Any],
    mode: str | None = None,
    only_probe: str | None = None,
    timeout_s: float = DEFAULT_PROBE_TIMEOUT_S,
) -> dict[str, Any]:
    case_dir = case_dir.resolve()
    work = _new_scratch(case_dir, "acd-probes-")
    log_dir = work / "logs"
    probes: list[dict[str, Any]] = []
    timeout_s = float(timeout_s)
    if timeout_s <= 0:
        raise ValueError("probe timeout must be positive")
    compile_cmd = _first_compile_command(context, case_dir=case_dir)
    selected = only_probe or "direct_kernel_compile"
    specs = [("probe_direct_kernel_compile", "direct_kernel_compile", None, False)]
    if mode == "optimization" and not only_probe:
        specs.extend(
            (f"probe_{opt}", "optimization_sensitivity", f"-{opt}", False)
            for opt in ("O0", "O2", "O3")
        )
    elif selected in {
        "O0",
        "O2",
        "O3",
        "o0",
        "o2",
        "o3",
        "probe_O0",
        "probe_O2",
        "probe_O3",
    }:
        opt = selected.removeprefix("probe_").upper()
        specs = [(f"probe_{opt}", "optimization_sensitivity", f"-{opt}", False)]
    elif selected in {"preprocess", "probe_preprocess_only"}:
        specs = [("probe_preprocess_only", "preprocess", None, True)]
    elif selected not in {"direct_kernel_compile", "probe_direct_kernel_compile"}:
        specs = []
    for name, evidence_kind, opt, preprocess in specs:
        try:
            if compile_cmd is None:
                raise ValueError(
                    "No captured kernel compile command uniquely identifies the failed translation unit."
                )
            probe_work = work / name
            probe_work.mkdir()
            command, cwd = _prepare_compile_probe(
                compile_cmd, case_dir, probe_work, opt=opt, preprocess=preprocess
            )
            probe = _run_probe(
                name,
                command,
                cwd,
                log_dir,
                timeout=timeout_s,
                evidence_kind=evidence_kind,
                tool_family=_tool_family_from_command(command),
            )
            probe.update(
                {
                    "argv": command,
                    "cwd": str(cwd),
                    "source": compile_cmd["file"],
                    "scratch": str(probe_work),
                }
            )
            probes.append(probe)
        except (ValueError, OSError) as exc:
            probes.append(
                _skipped_probe(
                    name,
                    reason=str(exc),
                    evidence_kind=evidence_kind,
                    timeout_s=timeout_s,
                )
            )
    if only_probe in {"msobjdump", "probe_msobjdump"}:
        msobjdump = which("msobjdump")
        artifacts = [
            p
            for p in case_dir.rglob("*")
            if p.is_file() and p.suffix in {".o", ".so", ".a", ""}
        ]
        if msobjdump and artifacts:
            probes.append(
                _run_probe(
                    "probe_msobjdump",
                    [msobjdump, "-h", str(artifacts[0])],
                    work,
                    log_dir,
                    timeout=timeout_s,
                    evidence_kind="artifact_metadata",
                    tool_family="msobjdump",
                )
            )
        else:
            probes.append(
                _skipped_probe(
                    "probe_msobjdump",
                    reason="msobjdump or artifact not found",
                    evidence_kind="artifact_metadata",
                    tool_family="msobjdump",
                    timeout_s=timeout_s,
                )
            )

    if only_probe in {"msopgen_regenerate", "probe_msopgen_regenerate"}:
        msopgen = which("msopgen") or which("msOpGen")
        json_out = log_dir / "probe_msopgen_regenerate.json"
        md_out = log_dir / "probe_msopgen_regenerate.md"
        started = time.perf_counter()
        try:
            isolated = _copy_isolated_source(case_dir, work / "regenerate-source")
            poc = run_msopgen_regenerate_poc(
                case_dir=isolated,
                json_out=json_out,
                md_out=md_out,
                explicit_tool=msopgen,
                out_dir=work / "regenerated",
                soc=os.environ.get("SOC_VERSION", "Ascend910B4"),
                command_timeout_s=timeout_s,
            )
        except (ValueError, OSError) as exc:
            probes.append(
                _skipped_probe(
                    "probe_msopgen_regenerate",
                    reason=str(exc),
                    evidence_kind="regenerate_diff",
                    tool_family="msopgen",
                    timeout_s=timeout_s,
                )
            )
            return {
                "probes": probes,
                "interpretation": _interpret_probes(case_dir, context, probes),
                "scratch": str(work),
            }
        elapsed_s = round(time.perf_counter() - started, 3)
        conclusion = poc.get("conclusion") or {}
        comparison = poc.get("comparison") or {}
        command = next(
            (
                cmd
                for cmd in poc.get("commands") or []
                if cmd.get("label") == "msopgen-gen"
            ),
            {},
        )
        probes.append(
            {
                "name": "probe_msopgen_regenerate",
                "command": command.get("command") or "",
                "status": _probe_status_from_msopgen_conclusion(
                    str(conclusion.get("status") or "")
                ),
                "returncode": command.get("returncode"),
                "log_path": str(md_out),
                "result_json": str(json_out),
                "result_md": str(md_out),
                "conclusion_status": conclusion.get("status"),
                "reason": conclusion.get("reason"),
                "matched_count": comparison.get("matched_count", 0),
                "changed_count": comparison.get("changed_count", 0),
                "unchanged_count": comparison.get("unchanged_count", 0),
                "elapsed_s": elapsed_s,
                "timeout_s": timeout_s,
                **_probe_metadata(
                    name="probe_msopgen_regenerate",
                    status=_probe_status_from_msopgen_conclusion(
                        str(conclusion.get("status") or "")
                    ),
                    command=command.get("command") or "",
                    evidence_kind="regenerate_diff",
                    tool_family="msopgen",
                ),
            }
        )

    if only_probe in {"msopst", "msopst_smoke", "probe_msopst_smoke"}:
        msopst = which("msopst") or which("msOpST")
        if msopst:
            probes.append(
                _run_probe(
                    "probe_msopst_smoke",
                    [msopst, "--help"],
                    case_dir,
                    log_dir,
                    timeout=timeout_s,
                    evidence_kind="environment",
                    tool_family="msopst",
                    invocation="forbidden",
                )
            )
        else:
            probes.append(
                _skipped_probe(
                    "probe_msopst_smoke",
                    reason="msopst not found",
                    evidence_kind="environment",
                    tool_family="msopst",
                    invocation="forbidden",
                    timeout_s=timeout_s,
                )
            )
    interpretation = _interpret_probes(case_dir, context, probes)
    return {"probes": probes, "interpretation": interpretation, "scratch": str(work)}


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run optional compiler probes for hard Ascend C failures."
    )
    parser.add_argument("--case", required=True)
    parser.add_argument("--context")
    parser.add_argument(
        "--mode",
        choices=["optimization"],
        help="Explicitly request optimization comparison; default replays only the failed recipe.",
    )
    parser.add_argument("--probe")
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_PROBE_TIMEOUT_S,
        help="Per-probe timeout seconds.",
    )
    parser.add_argument("--json-out", required=True)
    args = parser.parse_args()
    try:
        case_dir = Path(args.case).resolve()
        context = load_json(args.context, default={}) if args.context else {}
        data = run_probes(
            case_dir,
            context,
            mode=args.mode,
            only_probe=args.probe,
            timeout_s=args.timeout,
        )
    except Exception as exc:
        data = {
            "probes": [],
            "interpretation": {"likely": "unknown", "confidence": 0.0, "evidence": []},
            **error_payload("compiler_probe_runner", exc, case=args.case),
        }
    write_json(Path(args.json_out), data)
    print(str(Path(args.json_out)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
