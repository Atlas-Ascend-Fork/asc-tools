# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Observe compiler execution without changing the build's compiler arguments."""

from __future__ import annotations

import ast
import hashlib
import importlib.machinery
import importlib.util
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
import uuid
from pathlib import Path

_COMPILER_NAME = re.compile(
    r"^(?:[\w]+-)*(?:cc|c\+\+|gcc|g\+\+|clang|clang\+\+|bisheng|ccec)"
    r"(?:-[0-9]+(?:\.[0-9]+)*)?(?:\.exe)?$",
    re.IGNORECASE,
)


def _output_path(command: list[str], directory: Path) -> Path | None:
    for index, arg in enumerate(command[:-1]):
        if arg == "-o":
            path = Path(command[index + 1])
            return (path if path.is_absolute() else directory / path).resolve()
    return None


def _object_fingerprint(path: Path | None) -> dict | None:
    if path is None:
        return None
    try:
        stat = path.stat()
        if not path.is_file() or not stat.st_size:
            return None
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        return {
            "mtime_ns": stat.st_mtime_ns,
            "size": stat.st_size,
            "sha256": digest.hexdigest(),
        }
    except OSError:
        return None


def _begin_observation(command: list[str], directory: Path) -> dict:
    output = _output_path(command, directory)
    return {
        "argv": command,
        "directory": str(directory),
        "started_ns": time.time_ns(),
        "object": str(output) if output is not None else None,
        "object_before": _object_fingerprint(output),
    }


def _finish_observation(root: Path, value: dict, returncode: int) -> None:
    """Record the actual process result; observation failures never fail a build."""
    try:
        output = Path(value["object"]) if value.get("object") else None
        after = _object_fingerprint(output)
        row = {
            **value,
            "returncode": returncode,
            "ended_ns": time.time_ns(),
            "object_changed": after is not None and after != value.get("object_before"),
        }
        if after is not None:
            row["object_sha256"] = after["sha256"]
        root.mkdir(parents=True, exist_ok=True)
        (root / ("compiler-" + uuid.uuid4().hex + ".json")).write_text(
            json.dumps(row), encoding="utf-8"
        )
    except (OSError, ValueError):
        return


def instrument_python(env: dict, root: Path) -> dict:
    """Observe compiler children of Python build drivers, including absolute paths.

    Only the recipe's environment is changed. Existing sitecustomize is loaded
    first, and compiler argv, working directory, streams and exit status remain
    unchanged. Python -S/-I and replaced PYTHONPATH remain evidence gaps.
    """
    observer = root / "python-observer"
    observer.mkdir(parents=True, exist_ok=True)
    script = str(Path(__file__).resolve())
    bootstrap = (
        "import importlib.util as _acd_util\n"
        "import sys as _acd_sys\n"
        f"_acd_spec = _acd_util.spec_from_file_location('_acd_build_receipts', {script!r})\n"
        "_acd_module = _acd_util.module_from_spec(_acd_spec)\n"
        "_acd_sys.modules[_acd_spec.name] = _acd_module\n"
        "_acd_spec.loader.exec_module(_acd_module)\n"
        f"_acd_module.install_python_observer({str(root.resolve())!r}, {str(observer.resolve())!r})\n"
    )
    (observer / "sitecustomize.py").write_text(bootstrap, encoding="utf-8")
    previous = env.get("PYTHONPATH")
    python_path = str(observer.resolve()) + (os.pathsep + previous if previous else "")
    return {**env, "PYTHONPATH": python_path}


def instrument_compilers(env: dict, root: Path) -> dict:
    """Observe PATH-discovered CANN compilers, including native make children.

    Resolve the real executables before adding the private directory. Compiler
    arguments and recipe shell remain unchanged; hard-coded paths may bypass it.
    """
    binaries = {
        name: shutil.which(name, path=env.get("PATH")) for name in ("bisheng", "ccec")
    }
    shim = root / "bin"
    shim.mkdir(parents=True, exist_ok=True)
    for name, binary in binaries.items():
        if not binary or Path(binary).resolve() == (shim / name).resolve():
            continue
        prefix = shlex.join(
            [
                sys.executable,
                str(Path(__file__).resolve()),
                "--compiler",
                str(root),
                "--",
                binary,
            ]
        )
        wrapper = shim / name
        wrapper.write_text("#!/bin/sh\nexec " + prefix + ' "$@"\n', encoding="utf-8")
        wrapper.chmod(0o700)
    return {**env, "PATH": str(shim) + os.pathsep + env.get("PATH", "")}


def install_python_observer(root: str, observer_directory: str) -> None:
    """Install process observation in a build interpreter, preserving sitecustomize."""
    observer = Path(observer_directory).resolve()
    search = [p for p in sys.path if Path(p or os.curdir).resolve() != observer]
    previous = importlib.machinery.PathFinder.find_spec("sitecustomize", search)
    if previous is not None and previous.loader is not None:
        module = importlib.util.module_from_spec(previous)
        sys.modules["sitecustomize"] = module
        previous.loader.exec_module(module)
    original = subprocess.Popen
    if not isinstance(original, type) or getattr(
        original, "_acd_observes_compilers", False
    ):
        return
    receipt_root = Path(root)

    class ObservedPopen(original):
        _acd_observes_compilers = True
        _acd_original_popen = original

        def __init__(self, args, *positional, **kwargs):
            self._acd_observation = None
            self._acd_recorded = False
            # Positional Popen options can alter shell/executable/cwd. Observe
            # only the unambiguous list-argv interface used by build drivers.
            try:
                if (
                    not positional
                    and isinstance(args, (list, tuple))
                    and args
                    and all(isinstance(arg, (str, bytes, os.PathLike)) for arg in args)
                    and not kwargs.get("shell")
                    and kwargs.get("executable") is None
                ):
                    command = [os.fsdecode(arg) for arg in args]
                    if (
                        _COMPILER_NAME.fullmatch(Path(command[0]).name)
                        and "-c" in command
                        and Path(command[0]).resolve().parent
                        != (receipt_root / "bin").resolve()
                    ):
                        cwd = kwargs.get("cwd")
                        if cwd is None:
                            cwd = os.getcwd()  # portability: allow child cwd
                        directory = Path(os.fsdecode(cwd)).resolve()
                        self._acd_observation = _begin_observation(command, directory)
            except (OSError, TypeError, ValueError):
                # Recording is optional; preserve the original Popen behavior.
                self._acd_observation = None
            super().__init__(args, *positional, **kwargs)

        def _acd_finish(self, returncode):
            if (
                returncode is not None
                and self._acd_observation is not None
                and not self._acd_recorded
            ):
                self._acd_recorded = True
                _finish_observation(receipt_root, self._acd_observation, returncode)

        def wait(self, *args, **kwargs):
            result = super().wait(*args, **kwargs)
            self._acd_finish(result)
            return result

        def poll(self):
            result = super().poll()
            self._acd_finish(result)
            return result

    subprocess.Popen = ObservedPopen


def trace_command(argv: list[str], trace_path: Path) -> tuple[list[str], dict]:
    tracer = os.environ.get("ACD_STRACE") or shutil.which("strace")
    if os.name != "posix" or not tracer:
        return argv, {"status": "unavailable", "reason": "linux_strace_not_found"}
    try:
        check = subprocess.run(
            [
                tracer,
                "-e",
                "trace=none",
                "-o",
                str(trace_path.with_suffix(".check")),
                "--",
                "/bin/true",
            ],
            capture_output=True,
            timeout=5,
            check=False,
        )
        if check.returncode:
            return argv, {"status": "unavailable", "reason": "linux_ptrace_unavailable"}
    except (OSError, subprocess.TimeoutExpired):
        return argv, {"status": "unavailable", "reason": "linux_strace_probe_failed"}
    return [
        tracer,
        "-f",
        "-s",
        "65535",
        "-e",
        "trace=process,chdir,fchdir",
        "-o",
        str(trace_path),
        "--",
        *argv,
    ], {"status": "enabled", "path": str(trace_path)}


def read_receipts(path: Path, initial_cwd: Path) -> list[dict]:
    """Track successful exec/exit and inherited cwd, including unfinished clone lines."""
    if not path.is_file():
        return []
    working, pending, executions, exited = {}, {}, {}, set()
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"(?:\[pid\s+)?(\d+)\]?\s+(.*)", raw)
        if not match:
            continue
        pid, line = match[1], match[2]
        working.setdefault(pid, str(initial_cwd.resolve()))
        if "<unfinished ...>" in line:
            pending[pid] = line.split("<unfinished ...>")[0]
            continue
        resumed = re.match(r"<\.\.\. \w+ resumed>(.*)", line)
        if resumed:
            line = pending.pop(pid, "") + resumed[1]
        child = re.match(r"(?:clone|clone3|fork|vfork)\(.*\)\s*=\s*(\d+)\s*$", line)
        if child:
            working[child[1]] = working[pid]
        changed = re.match(r'chdir\(("(?:\\.|[^"\\])*")\)\s*=\s*0', line)
        if changed:
            try:
                value = Path(ast.literal_eval(changed[1]))
                working[pid] = str(
                    (
                        value if value.is_absolute() else Path(working[pid]) / value
                    ).resolve()
                )
            except (ValueError, SyntaxError, TypeError):
                working[pid] = None
        if line.startswith("fchdir(") and line.rstrip().endswith("= 0"):
            working[pid] = None
        execute = re.match(
            r'execve\(("(?:\\.|[^"\\])*"), (\[.*?\]), .*\)\s*=\s*0$', line
        )
        if execute:
            try:
                args = ast.literal_eval(execute[2])
                if (
                    isinstance(args, list)
                    and all(isinstance(a, str) for a in args)
                    and "-c" in args
                ):
                    executions[pid] = {
                        "argv": args,
                        "directory": working[pid],
                        "pid": pid,
                    }
            except (ValueError, SyntaxError):
                pass
        if line == "+++ exited with 0 +++":
            exited.add(pid)
    return [
        {**value, "returncode": 0} for pid, value in executions.items() if pid in exited
    ]


def receipt_matches(
    args: list[str],
    directory: Path,
    receipts: list[dict],
    *,
    object_sha256: str | None = None,
) -> bool:
    for receipt in receipts:
        actual = receipt.get("argv") or []
        if (
            not actual
            or not receipt.get("directory")
            or receipt.get("returncode") != 0
            or receipt.get("object_changed") is False
        ):
            continue
        if receipt.get("object_sha256") and receipt["object_sha256"] != object_sha256:
            continue
        if (
            Path(receipt["directory"]).resolve() == directory.resolve()
            and Path(actual[0]).name == Path(args[0]).name
            and actual[1:] == args[1:]
        ):
            return True
    return False


def instrument_cmake(env: dict, root: Path) -> dict:
    """Add private CMake and Python compiler observers to the recipe environment.

    Compiler discovery stays unchanged unless PATH forwarding is explicitly
    requested. Projects bypassing the observers retain an evidence gap.
    """
    if os.name != "posix":
        return env
    env = instrument_python(env, root)
    # Some recipes derive their SDK root from `command -v bisheng`. A compiler
    # shim changes that result even when it forwards exactly the original argv.
    # Keep such discovery intact by default; native-make forwarding is opt-in.
    if env.get("ACD_OBSERVE_COMPILER_PATH") == "1":
        env = instrument_compilers(env, root)
    cmake = shutil.which("cmake", path=env.get("PATH"))
    if not cmake:
        return env
    root.mkdir(parents=True, exist_ok=True)
    shim = root / "bin"
    shim.mkdir(exist_ok=True)
    script = Path(__file__).resolve()
    prefix = shlex.join([sys.executable, str(script), "--cmake", cmake, str(root)])
    wrapper = shim / "cmake"
    wrapper.write_text("#!/bin/sh\nexec " + prefix + ' "$@"\n', encoding="utf-8")
    wrapper.chmod(0o700)
    return {**env, "PATH": str(shim) + os.pathsep + env.get("PATH", "")}


def compiler_receipts(root: Path, started_ns: int) -> list[dict]:
    result = []
    if not root.is_dir():
        return result
    for path in root.glob("compiler-*.json"):
        try:
            row = json.loads(path.read_text(encoding="utf-8"))
            if (
                row.get("returncode") == 0
                and row.get("started_ns", 0) >= started_ns
                and row.get("object_changed") is not False
            ):
                result.append(row)
        except (OSError, ValueError):
            continue
    return result


def original_compiler_arguments(args: list[str]) -> list[str]:
    # Strip only this known instrumentation prefix, never arbitrary wrappers.
    while (
        len(args) > 5
        and Path(args[1]).resolve() == Path(__file__).resolve()
        and args[2] == "--compiler"
        and args[4] == "--"
    ):
        args = args[5:]
    return args


def _bracket(value: str) -> str:
    equals = "=="
    while "]" + equals + "]" in value:
        equals += "="
    return "[" + equals + "[" + value + "]" + equals + "]"


def _cmake_main(binary: str, root: Path, args: list[str]) -> int:
    passthrough = {
        "--build",
        "--install",
        "-E",
        "--version",
        "--help",
        "-P",
        "--find-package",
        "--preset",
        "--workflow",
        "--list-presets",
    }
    if args and not any(
        arg in passthrough or arg.startswith("--help-") for arg in args
    ):
        build = Path.cwd()  # portability: allow configure cwd
        for index, arg in enumerate(args):
            if arg == "-B" and index + 1 < len(args):
                build = Path(args[index + 1])
            elif arg.startswith("-B") and len(arg) > 2:
                build = Path(arg[2:])
        query = build / ".cmake" / "api" / "v1" / "query" / "codemodel-v2"
        query.parent.mkdir(parents=True, exist_ok=True)
        query.touch(exist_ok=True)
        previous = None
        for index, arg in enumerate(args):
            definition = (
                "-D" + args[index + 1] if arg == "-D" and index + 1 < len(args) else arg
            )
            match = re.match(r"-DCMAKE_PROJECT_INCLUDE(?::[^=]+)?=(.*)", definition)
            if match:
                previous = match[1]
        # Preserve an existing configure hook, including its execution order.
        if previous is None:
            cache = build / "CMakeCache.txt"
            if cache.is_file():
                found = re.search(
                    r"^CMAKE_PROJECT_INCLUDE:[^=]+=(.*)$",
                    cache.read_text(errors="replace"),
                    re.MULTILINE,
                )
                previous = found[1] if found else None
        hook = root / ("observe-" + uuid.uuid4().hex + ".cmake")
        launch = shlex.join(
            [
                sys.executable,
                str(Path(__file__).resolve()),
                "--compiler",
                str(root),
                "--",
            ]
        )
        lines = [f"include({_bracket(previous)})"] if previous else []
        lines += [
            "get_property(_acd_previous_launcher GLOBAL PROPERTY RULE_LAUNCH_COMPILE)",
            f"set(_acd_receipt_launcher {_bracket(launch)})",
            'set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${_acd_receipt_launcher} ${_acd_previous_launcher}")',
        ]
        hook.write_text("\n".join(lines) + "\n", encoding="utf-8")
        args = [*args, "-DCMAKE_PROJECT_INCLUDE=" + str(hook)]
    return subprocess.call([binary, *args])


def main() -> int:
    args = sys.argv[1:]
    if args and args[0] == "--compiler":
        root, command = Path(args[1]), args[3:]
        if "-c" not in command:
            if os.name == "posix":
                os.execvpe(command[0], command, os.environ)
            return subprocess.call(command)
        cwd = Path.cwd()  # portability: allow compiler cwd
        value = _begin_observation(command, cwd)
        popen = getattr(subprocess.Popen, "_acd_original_popen", subprocess.Popen)
        child = popen(command)
        previous_handlers = {}

        def forward_signal(signum, _frame):
            if child.poll() is None:
                child.send_signal(signum)

        if os.name == "posix":
            for signum in (signal.SIGTERM, signal.SIGINT):
                previous_handlers[signum] = signal.signal(signum, forward_signal)
        try:
            result = child.wait()
        finally:
            for signum, handler in previous_handlers.items():
                signal.signal(signum, handler)
        _finish_observation(root, value, result)
        if result < 0 and os.name == "posix":
            signal.signal(-result, signal.SIG_DFL)
            os.kill(os.getpid(), -result)
        return result
    if args and args[0] == "--cmake":
        return _cmake_main(args[1], Path(args[2]), args[3:])
    raise SystemExit("Internal build receipt launcher requires --compiler or --cmake")


if __name__ == "__main__":
    raise SystemExit(main())
