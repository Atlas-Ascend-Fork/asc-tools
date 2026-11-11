# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Verification checkpoints; recovery never dispatches work.

Hashes detect changed evidence, not hostile forgery. Environment values are
not stored. Journals are owned local records, not a portable build cache.
"""

from __future__ import annotations

import contextlib
import contextvars
import hashlib
import os
import re
import shlex
import shutil
import time
import uuid
from pathlib import Path

from harness.engine.common import load_json, write_json
from harness.engine.execution import process_identity
from harness.engine.verification_evidence import digest, source_state

CURRENT = contextvars.ContextVar("verification_journal", default=None)
ADAPTER = contextvars.ContextVar("verification_adapter", default={})
_SKIP = {
    ".git",
    ".venv",
    "venv",
    "__pycache__",
    "node_modules",
    "logs",
    "verification_stages",
}
_SUFFIXES = {
    ".asc",
    ".cce",
    ".cpp",
    ".cc",
    ".cxx",
    ".c",
    ".h",
    ".hpp",
    ".hh",
    ".cmake",
    ".sh",
    ".py",
    ".json",
    ".yaml",
    ".yml",
    ".txt",
    ".rsp",
    ".response",
    ".d",
    ".o",
    ".obj",
    ".make",
    ".ninja",
}


def file_hash(path: Path) -> str | None:
    try:
        h = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                h.update(block)
        return h.hexdigest()
    except OSError:
        return None


def tool_identity() -> str:
    root = Path(__file__).resolve().parent
    files = list(root.glob("*.py"))
    files.extend((root.parent / "runtime").glob("*"))
    files.extend((root.parent / "knowledge").rglob("*"))
    return digest(
        {
            p.relative_to(root.parent).as_posix(): file_hash(p)
            for p in sorted(files)
            if p.is_file() and "__pycache__" not in p.parts
        }
    )


def input_state(root: Path) -> dict:
    # The adapter is selected by this metadata, which source_state deliberately
    # omits from diagnostic source identities. Bind it in the request identity.
    return {**source_state(root), "manifest_sha256": file_hash(root / "manifest.json")}


def environment_identity(env=None) -> str:
    env = os.environ if env is None else env
    keys = (
        "PATH",
        "LD_LIBRARY_PATH",
        "CPATH",
        "CPLUS_INCLUDE_PATH",
        "LIBRARY_PATH",
        "CC",
        "CXX",
        "CMAKE_PREFIX_PATH",
    )
    selected = {
        k: v
        for k, v in env.items()
        if k in keys or k.startswith(("ASCEND", "CANN", "CATLASS"))
    }
    compilers = {}
    for name in (
        "cc",
        "c++",
        "gcc",
        "g++",
        "clang",
        "clang++",
        "bisheng",
        "ccec",
        "cmake",
    ):
        path = shutil.which(name, path=env.get("PATH"))
        if path:
            stat = Path(path).stat()
            compilers[name] = [
                str(Path(path).resolve()),
                stat.st_size,
                stat.st_mtime_ns,
            ]
    return digest({"environment": selected, "compilers": compilers})


def save(path: Path, payload: dict) -> None:
    def redact(value):
        if isinstance(value, dict):
            return {
                k: (
                    v
                    if set(v) == {"keys", "sha256"} and isinstance(v["keys"], list)
                    else {"keys": sorted(v), "sha256": digest(v)}
                )
                if k == "env_override" and isinstance(v, dict)
                else redact(v)
                for k, v in value.items()
            }
        if isinstance(value, list):
            return [redact(v) for v in value]
        return value

    payload = redact(payload)
    write_json(
        path, {"schema_version": "1.0", "sha256": digest(payload), "payload": payload}
    )


def read(path: Path) -> dict:
    data = load_json(path)
    if (
        not isinstance(data, dict)
        or data.get("schema_version") != "1.0"
        or not isinstance(data.get("payload"), dict)
        or digest(data["payload"]) != data.get("sha256")
    ):
        raise ValueError("verification checkpoint missing, invalid or changed")
    return data["payload"]


def update(**fields) -> None:
    current = CURRENT.get()
    if current:
        current["state"].update(fields)
        save(current["directory"] / "session.json", current["state"])


def execution_observed(request: dict, invoked: bool | None, status: str) -> None:
    """Keep dispatch facts even when the disk can no longer accept a checkpoint.

    None is an ambiguous launch window, never evidence that no build ran.
    Durable request/running/result receipts remain the cross-process authority.
    """
    current = CURRENT.get()
    if current:
        runs = current["state"].setdefault("execution_observations", {})
        runs[request["run_dir"]] = {
            "run_dir": request["run_dir"],
            "identity": request["identity"],
            "build_invoked": invoked,
            "status": status,
        }


def failure_context(directory: Path, state: dict) -> dict:
    runs = list(state.get("execution_observations", {}).values())
    invoked = (
        True
        if any(x["build_invoked"] is True for x in runs)
        else None
        if any(x["build_invoked"] is None for x in runs)
        else False
    )
    return {
        "directory": str(directory),
        "build_invoked": invoked,
        "executions": runs,
        "active_stage": state.get("active_stage"),
        "new_build_invoked": False,
    }


@contextlib.contextmanager
def session(directory: Path, request: dict):
    directory.mkdir(parents=True, exist_ok=False)
    state = {
        "request": request,
        "created_ns": time.time_ns(),
        "owner": process_identity(os.getpid()),
        "owner_finished": False,
        "tool_identity": tool_identity(),
        "environment_identity": environment_identity(),
        "stages": [],
        "input": input_state(Path(request["case_dir"])),
    }
    token = CURRENT.set({"directory": directory, "state": state})
    failure = None
    try:
        update()
        yield directory
    except BaseException as exc:
        failure = exc
        exc.verification_failure = failure_context(directory, state)
        raise
    finally:
        try:
            update(owner_finished=True)
        except Exception as exc:
            if failure is not None:
                # Preserve the original cause and the in-memory dispatch facts.
                failure.verification_failure["checkpoint_error"] = type(exc).__name__
            else:
                exc.verification_failure = failure_context(directory, state)
                raise
        finally:
            CURRENT.reset(token)


@contextlib.contextmanager
def adapter(metadata: dict):
    token = ADAPTER.set(metadata)
    try:
        yield
    finally:
        ADAPTER.reset(token)


def prepare_stage(log: Path, context: dict) -> Path:
    path = log.parent / "verification_stages" / uuid.uuid4().hex / "context.json"
    context["adapter"] = ADAPTER.get()
    context["tool_identity"] = tool_identity()
    context["environment_identity"] = environment_identity()
    save(path, context)
    current = CURRENT.get()
    if current:
        update(active_stage=str(path), stages=[*current["state"]["stages"], str(path)])
    return path


def bind_execution(path: Path, request: dict) -> None:
    # Written before worker creation; an ambiguous start can only be observed.
    save(path.with_name("execution.json"), request)


def evidence_snapshot(context: dict) -> dict:
    """Bounded hashes of source, generated inputs, receipts and dependency files."""
    paths, errors = set(), []
    for root in {
        context["build_root"],
        context["compile_source_root"],
        context["receipt_root"],
    }:
        for directory, dirs, files in os.walk(root, followlinks=False):
            dirs[:] = sorted(d for d in dirs if d not in _SKIP)
            for name in sorted(files):
                p = Path(directory) / name
                if p.suffix.lower() in _SUFFIXES or name in {"Makefile", "makefile"}:
                    paths.add(p.resolve())
                    if len(paths) > 50000:
                        return {"status": "partial", "reason": "file_budget"}
    paths.add(Path(context["trace_path"]))
    # Depfiles can name SDK headers outside the owned source tree. Parse as data.
    for p in sorted(paths):
        if p.suffix != ".d":
            continue
        try:
            if p.stat().st_size > 4 * 1024 * 1024:
                errors.append(str(p))
                continue
            body = re.split(
                r":\s+", p.read_text(encoding="utf-8").replace("\\\n", " "), 1
            )[1]
            for name in shlex.split(body):
                if name.endswith(":"):  # GCC -MP phony target
                    continue
                dep = Path(name)
                if not dep.is_absolute():
                    dep = Path(context["build_root"]) / dep
                paths.add(dep.resolve())
        except (OSError, UnicodeError, ValueError, IndexError):
            errors.append(str(p))
    hashes, total = {}, 0
    for p in sorted(paths):
        if len(hashes) >= 50000:
            return {"status": "partial", "reason": "file_budget"}
        try:
            stat = p.stat()
            total += stat.st_size
            if total > 1024 * 1024 * 1024:
                return {"status": "partial", "reason": "byte_budget"}
            fingerprint = file_hash(p)
            hashes[str(p)] = {
                "sha256": fingerprint,
                "size": stat.st_size,
                "mtime_ns": stat.st_mtime_ns,
            }
            if fingerprint is None:
                errors.append(str(p))
        except OSError:
            if str(p) != context["trace_path"]:
                errors.append(str(p))
            hashes[str(p)] = None
    return {
        "status": "partial" if errors else "complete",
        "files": hashes,
        "errors": sorted(set(errors)),
    }


def capture_witness(path: Path) -> dict:
    """Called by the existing worker after child exit, before its final receipt."""
    started_ns = time.time_ns()
    context = read(path)
    witness = {
        "context_sha256": digest(context),
        "evidence": evidence_snapshot(context),
        "source": source_state(Path(context["compile_source_root"])),
        "captured_ns": time.time_ns(),
    }
    witness["capture_duration_ns"] = witness["captured_ns"] - started_ns
    target = path.with_name("witness.json")
    save(target, witness)
    return {"path": str(target), "sha256": file_hash(target)}
