# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Durable execution receipts. No repair loop and no model or benchmark dependency."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import subprocess
import sys
import time
import uuid
from pathlib import Path

from harness.engine.common import PROJECT_ROOT, load_json, write_json


def _identity(data: dict) -> str:
    fields = {k: data.get(k) for k in ("argv", "cwd", "source", "environment_sha256")}
    return hashlib.sha256(json.dumps(fields, sort_keys=True).encode()).hexdigest()


def process_identity(pid: int) -> dict:
    identity = {"pid": pid, "platform": sys.platform}
    if sys.platform.startswith("linux"):
        try:
            stat = Path(f"/proc/{pid}/stat").read_text()
            fields = stat[stat.rfind(")") + 2 :].split()
            identity.update(
                start_ticks=fields[19],
                state=fields[0],
                boot_id=Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
            )
        except (OSError, IndexError):
            identity["unavailable"] = True
    return identity


def _alive(identity: dict) -> bool | None:
    if not identity.get("pid") or not identity.get("start_ticks"):
        return None
    current = process_identity(identity["pid"])
    return (
        not current.get("unavailable")
        and current.get("state") != "Z"
        and all(current.get(k) == identity.get(k) for k in ("start_ticks", "boot_id"))
    )


def observe_run(run_dir: Path, *, expected_identity: str | None = None) -> dict:
    """Observation never starts/restarts a build, including unknown start windows."""
    request = load_json(run_dir / "request.json", default={}) or {}
    if not request:
        return {"status": "unknown", "reason": "request_missing", "build_invoked": None}
    if (
        request.get("schema_version") != "2.0"
        or not request.get("environment_sha256")
        or _identity(request) != request.get("identity")
    ):
        return {"status": "unknown", "reason": "request_changed", "build_invoked": None}
    if expected_identity and request.get("identity") != expected_identity:
        return {
            "status": "unknown",
            "reason": "identity_mismatch",
            "build_invoked": None,
        }
    result = load_json(run_dir / "result.json", default={}) or {}
    if result:
        if (
            result.get("identity") != request.get("identity")
            or _identity(result) != request.get("identity")
            or result.get("run_id") != request.get("run_id")
        ):
            return {
                "status": "unknown",
                "reason": "receipt_mismatch",
                "build_invoked": None,
            }
        if result.get("status") == "passed" and (
            result.get("returncode") != 0
            or result.get("log_capture") != "complete"
            or not result.get("build_invoked")
        ):
            return {
                **result,
                "status": "unknown",
                "reason": "terminal_receipt_inconsistent",
            }
        log = Path(result["log_path"])
        try:
            digest = hashlib.sha256(log.read_bytes()).hexdigest()
        except OSError:
            digest = None
        if digest != result.get("log_sha256"):
            return {
                **result,
                "status": "unknown",
                "log_capture": "invalid",
                "reason": "log_missing_or_changed",
            }
        return {**result, "observed": True}
    running = load_json(run_dir / "running.json", default={}) or {}
    alive = _alive(running.get("worker", {}))
    return {
        **request,
        "status": "running"
        if alive
        else "interrupted"
        if alive is False
        else "unknown",
        "build_invoked": True if running.get("child") else None,
        "log_path": str(run_dir / "build.log"),
        "log_capture": "partial",
        "reason": "no_terminal_receipt",
        "process": running,
    }


def _stop(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    if os.name == "posix":
        os.killpg(proc.pid, signal.SIGTERM)
    else:
        proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        if os.name == "posix":
            os.killpg(proc.pid, signal.SIGKILL)
        else:
            proc.kill()
        proc.wait(timeout=5)


def _worker(run_dir: Path) -> int:
    request = load_json(run_dir / "request.json")
    if _identity(request) != request.get("identity"):
        return 2
    running = {
        "worker": process_identity(os.getpid()),
        "run_id": request["run_id"],
        "started_ns": time.time_ns(),
    }
    write_json(run_dir / "running.json", running)
    result = {
        **request,
        "started_ns": running["started_ns"],
        "log_path": str(run_dir / "build.log"),
        "build_invoked": False,
        "returncode": None,
        "log_capture": "partial",
    }
    proc = None
    try:
        # Child writes directly to disk; controller lifetime cannot lose buffered PIPE output.
        with (run_dir / "build.log").open("wb", buffering=0) as stream:
            proc = subprocess.Popen(
                request["argv"],
                cwd=request["cwd"],
                stdout=stream,
                stderr=subprocess.STDOUT,
                start_new_session=os.name == "posix",
            )
            result["build_invoked"] = True
            running["child"] = process_identity(proc.pid)
            write_json(run_dir / "running.json", running)
            try:
                code = proc.wait(timeout=request["timeout"])
                result.update(
                    returncode=code, status="passed" if code == 0 else "failed"
                )
            except subprocess.TimeoutExpired:
                _stop(proc)
                result.update(
                    status="interrupted",
                    returncode=proc.returncode,
                    interruption_reason="timeout",
                )
            stream.flush()
            os.fsync(stream.fileno())
        result["log_capture"] = "complete"
        result["log_sha256"] = hashlib.sha256(
            (run_dir / "build.log").read_bytes()
        ).hexdigest()
    except (OSError, ValueError) as exc:
        if proc is not None:
            _stop(proc)
        result.update(
            status="unknown" if proc else "not_started",
            verify_infra_error=type(exc).__name__,
            error=str(exc),
        )
    result["ended_ns"] = time.time_ns()
    if request.get("verification_context"):
        try:
            from harness.engine.verification_recovery import capture_witness

            result["verification_witness"] = capture_witness(
                Path(request["verification_context"])
            )
        except (OSError, ValueError, TypeError) as exc:
            result["verification_witness_error"] = type(exc).__name__
    write_json(run_dir / "result.json", result)
    lock = run_dir.parent / "active.json"
    owner = load_json(lock, default={}) or {}
    if (
        owner.get("run_dir") == str(run_dir)
        and owner.get("identity") == request["identity"]
    ):
        lock.unlink(missing_ok=True)
    return 0


def execute_recipe(
    argv: list[str],
    cwd: Path,
    log_path: Path,
    *,
    timeout: float,
    env: dict | None = None,
    source_identity: str | None = None,
    verification_context: Path | None = None,
) -> dict:
    if timeout <= 0:
        raise ValueError("timeout must be positive")
    if not argv or not all(isinstance(x, str) for x in argv):
        raise ValueError("argv must be a nonempty string array")
    root = log_path.parent / (log_path.name + ".runs")
    root.mkdir(parents=True, exist_ok=True)
    # Serialize this logical invocation. A stale lock is an explicit unknown, never a restart.
    lock = root / "active.json"
    try:
        fd = os.open(lock, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        return {
            "status": "unknown",
            "build_invoked": False,
            "returncode": None,
            "log_path": str(log_path),
            "verify_infra_error": "active_or_unresolved_run",
            "active_record": str(lock),
        }
    run_id = uuid.uuid4().hex
    run_dir = root / run_id
    run_dir.mkdir()
    worker_env = dict(os.environ if env is None else env)
    worker_env["PYTHONPATH"] = (
        str(PROJECT_ROOT) + os.pathsep + worker_env.get("PYTHONPATH", "")
    )
    worker_env["PYTHONDONTWRITEBYTECODE"] = "1"
    # Bind the complete environment passed to the worker, without persisting values.
    environment_sha256 = hashlib.sha256(
        json.dumps(worker_env, sort_keys=True, ensure_ascii=True).encode()
    ).hexdigest()
    identity_data = {
        "argv": argv,
        "cwd": str(cwd.resolve()),
        "source": source_identity,
        "environment_sha256": environment_sha256,
    }
    identity = _identity(identity_data)
    request = {
        "schema_version": "2.0",
        "run_id": run_id,
        "identity": identity,
        **identity_data,
        "timeout": timeout,
        "created_ns": time.time_ns(),
        "run_dir": str(run_dir),
    }
    if verification_context is not None:
        request["verification_context"] = str(verification_context)
    with os.fdopen(fd, "w", encoding="utf-8") as stream:
        json.dump({"run_dir": str(run_dir), "identity": identity}, stream)
    write_json(run_dir / "request.json", request)
    if verification_context is not None:
        from harness.engine.verification_recovery import bind_execution

        bind_execution(verification_context, request)
    from harness.engine.verification_recovery import execution_observed

    worker = None
    try:
        with (run_dir / "worker.log").open("wb") as worker_log:
            execution_observed(request, None, "launch_window")
            worker = subprocess.Popen(
                [
                    sys.executable,
                    "-m",
                    "harness.engine.execution",
                    "--worker",
                    str(run_dir),
                ],
                env=worker_env,
                stdout=worker_log,
                stderr=subprocess.STDOUT,
                start_new_session=os.name == "posix",
            )
        worker.wait(timeout=timeout + 20)
    except subprocess.TimeoutExpired:
        result = observe_run(run_dir)
        execution_observed(
            request, True if result.get("build_invoked") else None, result["status"]
        )
        return {**result, "run_dir": str(run_dir)}
    except OSError as exc:
        # wait()/log-close can fail after Popen. Never call that "not started".
        invoked = False if worker is None else None
        execution_observed(
            request, invoked, "not_started" if worker is None else "unknown"
        )
        return {
            "status": "not_started" if worker is None else "unknown",
            "build_invoked": invoked,
            "returncode": None,
            "log_path": str(run_dir / "build.log"),
            "run_dir": str(run_dir),
            "verify_infra_error": type(exc).__name__,
        }
    result = observe_run(run_dir)
    execution_observed(
        request,
        result.get("build_invoked")
        if result.get("status") in {"passed", "failed", "not_started", "interrupted"}
        and (run_dir / "result.json").is_file()
        else True
        if result.get("build_invoked")
        else None,
        result["status"],
    )
    # Only a terminal receipt can release the logical invocation.
    if (run_dir / "result.json").is_file():
        owner = load_json(lock, default={}) or {}
        if owner.get("run_dir") == str(run_dir):
            lock.unlink(missing_ok=True)
    return {**result, "run_dir": str(run_dir)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker", type=Path)
    parser.add_argument("--observe", type=Path)
    args = parser.parse_args()
    if args.worker:
        return _worker(args.worker)
    if args.observe:
        print(json.dumps(observe_run(args.observe), ensure_ascii=False))
        return 0
    parser.error("choose --observe")


if __name__ == "__main__":
    raise SystemExit(main())
