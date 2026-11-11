# ----------------------------------------------------------------------------------------------------------
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Write local JSONL audit events with configurable storage and deterministic redaction."""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import time
from contextlib import contextmanager
from contextvars import ContextVar
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Iterator

from harness.engine.common import PROJECT_ROOT

__all__ = [
    "DEFAULT_AUDIT_DIR",
    "AuditEvent",
    "write_audit",
    "iter_events",
    "tail_events",
    "redact_for_audit",
    "audit_location",
    "emit_audit",
    "resolve_audit_dir",
]


DEFAULT_AUDIT_DIR: Path = (
    (
        Path(os.environ.get("LOCALAPPDATA") or Path.home() / "AppData" / "Local")
        if os.name == "nt"
        else Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local" / "state")
    )
    / "ascendc-compile-doctor"
    / "audit"
)
_location: ContextVar = ContextVar("acd_audit_location", default=None)
_emitting: ContextVar = ContextVar("acd_audit_emitting", default=False)


def _within(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def _check_location(path: Path, roots: Iterable[Path]) -> Path:
    path = path.expanduser().resolve()
    if any(_within(path, Path(root).resolve()) for root in roots):
        raise ValueError("audit_location_inside_protected_tree")
    return path


def resolve_audit_dir(audit_dir: Path | None = None) -> Path:
    """Explicit CLI directories remain supported; public contexts cannot be bypassed."""
    bound = _location.get()
    if bound is not None:
        selected, roots = bound
        return _check_location(
            Path(audit_dir) if audit_dir is not None else selected, roots
        )
    if audit_dir is not None:
        return Path(audit_dir).expanduser().resolve()
    selected = Path(os.environ.get("ACD_AUDIT_DIR") or DEFAULT_AUDIT_DIR)
    return _check_location(selected, (PROJECT_ROOT,))


@contextmanager
def audit_location(
    *, output_dir: Path | None = None, forbidden_roots: Iterable[Path] = ()
):
    """Bind all public hooks before dispatch, without changing process environment.

    An explicit environment directory is validated, never silently replaced.
    Outputs inside a source tree use independent state instead of adding source files.
    No directories are created by selection alone (including --help).
    """
    roots = tuple(Path(root).resolve() for root in forbidden_roots) + (
        PROJECT_ROOT.resolve(),
    )
    explicit = os.environ.get("ACD_AUDIT_DIR")
    selected = (
        Path(explicit)
        if explicit
        else (output_dir / "audit" if output_dir else DEFAULT_AUDIT_DIR)
    )
    if not explicit and any(_within(selected.resolve(), root) for root in roots):
        selected = DEFAULT_AUDIT_DIR
    selected = _check_location(selected, roots)
    token = _location.set((selected, roots))
    try:
        yield selected
    finally:
        _location.reset(token)


def emit_audit(event: str, payload: dict[str, Any]) -> dict[str, Any]:
    """Best effort with visible, non-sensitive failure; prevent PathGuard recursion."""
    if _emitting.get():
        return {"status": "failed", "reason": "audit_write_reentrant"}
    token = _emitting.set(True)
    try:
        target = write_audit(event, payload)
        return {"status": "written", "path": str(target)}
    except Exception as exc:
        kind = type(exc).__name__
        sys.stderr.write(f"[audit] audit_write_failed ({kind})\n")
        return {"status": "failed", "reason": "audit_write_failed", "error_type": kind}
    finally:
        _emitting.reset(token)


def _audit_file_for(ts: float, audit_dir: Path) -> Path:
    day = datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y%m%d")
    return audit_dir / f"audit_{day}.jsonl"


@dataclass
class AuditEvent:
    ts: str
    ts_epoch: float
    event: str
    pid: int
    user: str
    host: str
    cwd: str
    payload: dict[str, Any]

    def to_jsonl(self) -> str:
        return json.dumps(
            {
                "ts": self.ts,
                "ts_epoch": self.ts_epoch,
                "event": self.event,
                "pid": self.pid,
                "user": self.user,
                "host": self.host,
                "cwd": self.cwd,
                "payload": self.payload,
            },
            ensure_ascii=False,
            sort_keys=False,
        )


def redact_for_audit(value: Any) -> Any:
    """对 audit 字段跑 redactor 不可逆脱敏，失败仅保留占位，不回退原值。

    单独抽出来让 PathGuard 钩子在 redactor 不可用时仍能写最简事件。
    """
    try:
        from harness.engine.redactor import redact_obj
        from harness.engine.redactor import redact_text

        if isinstance(value, str):
            redacted, _ = redact_text(value)
            return redacted
        redacted, _ = redact_obj(value)
        return redacted
    except Exception:
        sys.stderr.write("[audit] audit_redaction_unavailable; field omitted\n")
        return (
            {"_redaction_unavailable": True}
            if isinstance(value, dict)
            else "[REDACTION_UNAVAILABLE]"
        )


def _current_user() -> str:
    for env_var in ("USERNAME", "USER", "LOGNAME"):
        v = os.environ.get(env_var)
        if v:
            return v
    return "unknown"


def _current_host() -> str:
    try:
        return socket.gethostname() or "unknown"
    except Exception:
        return "unknown"


def write_audit(
    event: str,
    payload: dict[str, Any] | None = None,
    *,
    audit_dir: Path | None = None,
    timestamp: float | None = None,
    redact: bool = True,
) -> Path:
    """追加一条 jsonl 事件，返回写入的文件路径。

    - ``event``：事件类型（短 snake_case，建议如 ``path_guard_blocked`` /
      ``redact_run`` / ``verify_invoked``）。
    - ``payload``：自定义字段；不要塞超大 blob（>4KB 自动截断）。
    - ``audit_dir``：显式开发路径；否则用公开调用绑定、ACD_AUDIT_DIR 或用户状态目录。
    - ``redact``：True 时对 user/host/cwd/payload 都跑 redactor。
    """
    ts_epoch = time.time() if timestamp is None else timestamp
    ts = datetime.fromtimestamp(ts_epoch, tz=timezone.utc).isoformat(
        timespec="microseconds"
    )
    audit_dir = resolve_audit_dir(audit_dir)
    audit_dir.mkdir(parents=True, exist_ok=True, mode=0o700)

    user = _current_user()
    host = _current_host()
    cwd = os.getcwd()  # portability: allow audit records the invoking process's cwd
    body: dict[str, Any] = dict(payload or {})

    if redact:
        # Replace user and host values explicitly in addition to path redaction.
        # When redaction is disabled, retain the original local values.
        user = "$USER" if user else user
        host = "$HOST" if host else host
        cwd = redact_for_audit(cwd) if cwd else cwd
        body = redact_for_audit(body) if body else body

    body_json = json.dumps(body, ensure_ascii=False)
    if len(body_json.encode("utf-8")) > 4096:
        body = {"_truncated": True, "_preview": body_json[:512]}

    evt = AuditEvent(
        ts=ts,
        ts_epoch=ts_epoch,
        event=event,
        pid=os.getpid(),
        user=user,
        host=host,
        cwd=cwd,
        payload=body,
    )
    target = _audit_file_for(ts_epoch, audit_dir)
    # A pre-existing daily-file symlink must not redirect audit into another tree.
    if target.resolve().parent != audit_dir:
        raise ValueError("audit_file_outside_selected_directory")
    # Append one complete JSON line per write. Use an interprocess lock where
    # available to prevent concurrent writers from interleaving records.
    line = evt.to_jsonl() + "\n"
    with open(
        target,
        "a",
        encoding="utf-8",
        opener=lambda path, flags: os.open(path, flags | os.O_APPEND, 0o600),
    ) as fh:
        try:
            import fcntl

            fcntl.flock(fh.fileno(), fcntl.LOCK_EX)
        except (ImportError, OSError):
            fcntl = None  # Without fcntl, use one write on a file opened with O_APPEND.
        try:
            fh.write(line)
        finally:
            if fcntl is not None:
                try:
                    fcntl.flock(fh.fileno(), fcntl.LOCK_UN)
                except OSError:
                    pass
    return target


def iter_events(
    audit_dir: Path | None = None,
    *,
    since: datetime | None = None,
    files: Iterable[Path] | None = None,
) -> Iterator[dict[str, Any]]:
    """流式读 jsonl 事件。``since`` 按 ts_epoch 过滤。"""
    audit_dir = resolve_audit_dir(audit_dir)
    if files is None:
        if not audit_dir.exists():
            return
        files = sorted(audit_dir.glob("audit_*.jsonl"))
    since_epoch = since.timestamp() if since else None
    for file in files:
        try:
            with file.open("r", encoding="utf-8") as fh:
                for line in fh:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        evt = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if since_epoch is not None and evt.get("ts_epoch", 0) < since_epoch:
                        continue
                    yield evt
        except OSError:
            continue


def tail_events(n: int, audit_dir: Path | None = None) -> list[dict[str, Any]]:
    """返回最近 n 条事件（按写入顺序，跨日合并）。"""
    events = list(iter_events(audit_dir))
    return events[-n:] if n > 0 else events


def _parse_since(s: str) -> datetime:
    # Accept date-only and ISO-8601 timestamps.
    for fmt in ("%Y-%m-%d", "%Y-%m-%dT%H:%M:%S", "%Y-%m-%dT%H:%M:%S%z"):
        try:
            return (
                datetime.strptime(s, fmt).replace(tzinfo=timezone.utc)
                if "%z" not in fmt
                else datetime.strptime(s, fmt)
            )
        except ValueError:
            continue
    raise argparse.ArgumentTypeError(f"unrecognized timestamp: {s!r}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Read or append the local audit log.")
    sub = parser.add_subparsers(dest="cmd", required=False)

    p_tail = sub.add_parser("tail", help="Print last N events as JSON-lines.")
    p_tail.add_argument("-n", type=int, default=20)
    p_tail.add_argument("--audit-dir", type=Path, default=None)

    p_since = sub.add_parser("since", help="Print events with ts >= given timestamp.")
    p_since.add_argument("ts", type=_parse_since)
    p_since.add_argument("--audit-dir", type=Path, default=None)

    p_write = sub.add_parser(
        "write", help="Write a single audit event from CLI (mainly for tests)."
    )
    p_write.add_argument("event")
    p_write.add_argument("--payload-json", default="{}")
    p_write.add_argument("--audit-dir", type=Path, default=None)
    p_write.add_argument("--no-redact", action="store_true")

    args = parser.parse_args(argv)

    if args.cmd == "tail":
        for evt in tail_events(args.n, audit_dir=args.audit_dir):
            sys.stdout.write(json.dumps(evt, ensure_ascii=False) + "\n")
        return 0
    if args.cmd == "since":
        for evt in iter_events(audit_dir=args.audit_dir, since=args.ts):
            sys.stdout.write(json.dumps(evt, ensure_ascii=False) + "\n")
        return 0
    if args.cmd == "write":
        try:
            payload = json.loads(args.payload_json)
        except json.JSONDecodeError as exc:
            sys.stderr.write(f"invalid --payload-json: {exc}\n")
            return 2
        target = write_audit(
            args.event,
            payload if isinstance(payload, dict) else {"value": payload},
            audit_dir=args.audit_dir,
            redact=not args.no_redact,
        )
        sys.stdout.write(json.dumps({"wrote": str(target)}, ensure_ascii=False) + "\n")
        return 0

    parser.print_help()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
