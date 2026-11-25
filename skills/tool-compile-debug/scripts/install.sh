#!/usr/bin/env bash
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
# 用途：在当前项目、全局用户目录或自定义目录安装纯净版 tool-compile-debug 及共享 CANN 知识查询技能。
# 使用方法：bash scripts/install.sh [--clients codex,gemini,opencode,claude] [--project DIR | --global | --target DIR]
# Install tool-compile-debug and CANN knowledge query skills locally, globally, or to a custom directory.
# Self-contained so it can be downloaded and executed directly without cloning the repository first.
set -euo pipefail

python_bin="${CANNBOT_PYTHON:-}"
python_next=false
for argument in "$@"; do
  if [[ "$python_next" == true ]]; then
    python_bin="$argument"
    python_next=false
  elif [[ "$argument" == "--python" ]]; then
    python_next=true
  elif [[ "$argument" == --python=* ]]; then
    python_bin="${argument#--python=}"
  fi
done

is_python_ge_310() {
  "$1" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' >/dev/null 2>&1
}

if [[ -z "$python_bin" ]]; then
  for candidate in python3 python3.13 python3.12 python3.11 python3.10 python; do
    if command -v "$candidate" >/dev/null 2>&1 && is_python_ge_310 "$candidate"; then
      python_bin="$candidate"
      break
    fi
  done
  if [[ -z "$python_bin" ]]; then
    found=""
    if command -v python3 >/dev/null 2>&1; then
      found=" (found $(python3 --version 2>&1))"
    fi
    echo "Python 3.10 or newer is required${found}; select one with --python or CANNBOT_PYTHON." >&2
    exit 2
  fi
elif ! command -v "$python_bin" >/dev/null 2>&1; then
  echo "Configured Python interpreter is not executable: $python_bin" >&2
  exit 2
elif ! is_python_ge_310 "$python_bin"; then
  echo "Python 3.10 or newer is required (found $($python_bin --version 2>&1)); select one with --python." >&2
  exit 2
fi

exec "$python_bin" - "${BASH_SOURCE[0]:-}" "$@" <<'PY'
from __future__ import annotations

import argparse
import contextlib
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import unicodedata
from urllib.parse import urlsplit

try:
    import termios
    import tty
except ImportError:  # pragma: no cover
    termios = None
    tty = None

SKILL = "tool-compile-debug"
PACKAGE_REPOSITORY = "https://gitcode.com/cann/asc-tools.git"
PACKAGE_REF = "master"
QUERIES = ("knowledge-query", "ascendc-api-knowledge-query")
SHARED_CLIENTS = ("codex", "gemini", "opencode")
CLAUDE_CLIENT = "claude"
ALL_CLIENTS = (*SHARED_CLIENTS, CLAUDE_CLIENT)
ROOT_KEY = "CANNBOT_KNOWLEDGE_ROOT"
PLATFORM_KEY = "CANNBOT_KNOWLEDGE_PLATFORM"
COPY_IGNORE = {".git", "__pycache__", ".pytest_cache", ".DS_Store"}
COMMAND_TIMEOUT_SECONDS = 600

PINNED_KNOWLEDGE_LOCK = {
    "schema": "compile-debug-knowledge-dependencies-v1",
    "repository": "https://gitcode.com/cann/cannbot-knowledge.git",
    "commit": "c7609c5a17d86c25a593fa96ea64e1d9f9e7f2ca",
    "consumer_skills": ["knowledge-query", "ascendc-api-knowledge-query"],
    "skill_directory": ".agents/skills",
    "python_minimum": "3.10",
    "resources": "complete knowledge checkout; no source snapshots by default",
    "reference_workflow": "asc-tools MR !937, d6b88a93d5c5ac1e60a6e89ca1897e355dfd51ce",
}

PURE_TOP_LEVEL = frozenset({"SKILL.md", "references", "evals", "scripts"})
FORBIDDEN_PURE_PATHS = (
    "README.md",
    "CHANGELOG.md",
    "requirements.txt",
    ".gitattributes",
    "tests",
    "workspace",
    "scripts/engine/workspace",
    "scripts/install.sh",
    "scripts/install.py",
    "scripts/knowledge.lock.json",
)
REQUIRED_PURE_FILES = (
    "SKILL.md",
    "evals/evals.json",
    "evals/trigger-evals.json",
    "references/cannbot-contract.md",
    "references/using-scripts.md",
    "references/error-categories/README.md",
    "scripts/run.py",
    "scripts/preflight.py",
    "scripts/face_selfcheck.py",
    "scripts/diagnose_compile_failure.sh",
    "scripts/run_verify.sh",
    "scripts/redact_bundle.sh",
    "scripts/engine/__init__.py",
    "scripts/engine/README.md",
    "scripts/engine/harness/__init__.py",
    "scripts/engine/harness/runtime/build_one.sh",
    "scripts/engine/harness/engine/diagnosis_engine.py",
    "scripts/engine/harness/engine/diagnose_compile_failure.py",
    "scripts/engine/harness/engine/verify_runner.py",
    "scripts/engine/harness/engine/redactor.py",
    "scripts/engine/harness/config/versions.yaml",
    "scripts/engine/harness/config/error_signatures.yaml",
    "scripts/engine/harness/config/compile_mechanisms.yaml",
    "scripts/engine/harness/config/static_rules.yaml",
)


class UserCancelled(RuntimeError):
    """User canceled interactive installation."""


def color_enabled(stream):
    if os.environ.get("NO_COLOR"):
        return False
    try:
        return stream.isatty()
    except OSError:
        return False


class Style:
    def __init__(self, enabled):
        self.enabled = enabled

    def wrap(self, code, text):
        return f"\033[{code}m{text}\033[0m" if self.enabled else text

    def bold(self, text):
        return self.wrap("1", text)

    def dim(self, text):
        return self.wrap("2", text)

    def cyan(self, text):
        return self.wrap("36", text)

    def green(self, text):
        return self.wrap("32", text)

    def yellow(self, text):
        return self.wrap("33", text)


class TTYSession:
    """Interactive terminal prompts via /dev/tty (works even under `curl ... | bash`)."""

    def __init__(self):
        self._fd = None
        self.in_stream = None
        self.out_stream = sys.stdout
        self.can_raw = False
        self.style = Style(False)

    def open(self):
        try:
            self._fd = os.open("/dev/tty", os.O_RDWR)
            self.in_stream = os.fdopen(self._fd, "rb", buffering=0)
            self.out_stream = os.fdopen(
                os.dup(self._fd), "w", encoding="utf-8", buffering=1
            )
            self.style = Style(color_enabled(self.out_stream))
            if termios is not None and tty is not None:
                try:
                    termios.tcgetattr(self._fd)
                    self.can_raw = os.environ.get("TERM", "") not in ("", "dumb")
                except termios.error:
                    self.can_raw = False
            return True
        except OSError:
            self.close()
            return False

    def close(self):
        if self.in_stream is not None:
            try:
                self.in_stream.close()
            except OSError:
                pass
            self.in_stream = None
            self._fd = None
        if self.out_stream is not None and self.out_stream is not sys.stdout:
            try:
                self.out_stream.close()
            except OSError:
                pass
            self.out_stream = sys.stdout

    def write(self, text=""):
        self.out_stream.write(text)
        self.out_stream.flush()

    def writeln(self, text=""):
        self.write(text + "\n")

    def _terminal_cols(self):
        try:
            return max(40, os.get_terminal_size(self._fd).columns)
        except (OSError, TypeError):
            return 80

    @staticmethod
    def _truncate_display(text, max_cols):
        width = 0
        out = []
        for ch in text:
            ch_w = 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
            if width + ch_w > max_cols:
                if max_cols >= 3:
                    while out and width > max_cols - 3:
                        popped = out.pop()
                        width -= (
                            2
                            if unicodedata.east_asian_width(popped) in ("W", "F")
                            else 1
                        )
                    out.append("...")
                break
            out.append(ch)
            width += ch_w
        return "".join(out)

    def _format_option(self, prefix, label, desc, *, highlight):
        avail = max(20, self._terminal_cols() - 10)
        raw_desc = f" - {desc}" if desc else ""
        fitted = self._truncate_display(f"{label}{raw_desc}", avail)
        if fitted.startswith(label):
            rest = fitted[len(label) :]
            body = (self.style.bold(label) if highlight else label) + (
                self.style.dim(rest) if rest else ""
            )
        else:
            body = self.style.bold(fitted) if highlight else fitted
        return f"\r\033[2K  {prefix} {body}"

    def read_line(self, prompt):
        self.write(prompt)
        chunks = []
        while True:
            ch = self.in_stream.read(1)
            if not ch:
                raise UserCancelled("Installation canceled.")
            if ch in (b"\n", b"\r"):
                break
            chunks.append(ch)
        return b"".join(chunks).decode("utf-8", errors="replace").strip()

    def _read_key(self):
        ch = self.in_stream.read(1)
        if not ch or ch == b"\x03":
            return "CTRL_C"
        if ch in (b"\r", b"\n"):
            return "ENTER"
        if ch == b" ":
            return "SPACE"
        if ch == b"\x1b":
            nxt = self.in_stream.read(1)
            if nxt == b"[":
                code = self.in_stream.read(1)
                if code == b"A":
                    return "UP"
                if code == b"B":
                    return "DOWN"
            return "ESC"
        return ch.decode("utf-8", errors="ignore")

    def prompt_text(self, label, default=""):
        suffix = f" [{default}]" if default else ""
        raw = self.read_line(f"  {label}{suffix}: ")
        return raw if raw else default

    def prompt_confirm(self, message, default=True):
        suffix = "[Y/n]" if default else "[y/N]"
        while True:
            raw = self.read_line(f"{message} {suffix}: ").lower()
            if not raw:
                return default
            if raw in ("y", "yes"):
                return True
            if raw in ("n", "no"):
                return False
            self.writeln(self.style.yellow("  Please enter y or n."))

    def select_one(self, title, options, default_index=0):
        if not self.can_raw:
            self.writeln(self.style.bold(title))
            for idx, (label, desc) in enumerate(options, start=1):
                mark = " (default)" if idx - 1 == default_index else ""
                suffix = f" - {desc}" if desc else ""
                self.writeln(f"  {idx}) {label}{mark}{suffix}")
            while True:
                raw = self.read_line(f"  Select [default {default_index + 1}]: ")
                if not raw:
                    return default_index
                if raw.lower() == "q":
                    raise UserCancelled("Installation canceled.")
                if raw.isdigit() and 1 <= int(raw) <= len(options):
                    return int(raw) - 1
                self.writeln(
                    self.style.yellow(f"  Enter a number between 1 and {len(options)}.")
                )

        cursor = default_index
        rendered = 0
        self.writeln(self.style.bold(title))
        self.writeln(
            self.style.dim("  Keys: Up/Down to move, Enter to confirm, q to cancel")
        )
        old_attrs = termios.tcgetattr(self._fd)
        try:
            tty.setraw(self._fd, when=termios.TCSADRAIN)
            while True:
                if rendered:
                    self.write(f"\033[{rendered}A")
                lines = []
                for idx, (label, desc) in enumerate(options):
                    pointer = self.style.cyan(">") if idx == cursor else " "
                    marker = self.style.green("*") if idx == cursor else "o"
                    lines.append(
                        self._format_option(
                            f"{pointer} {marker}",
                            label,
                            desc,
                            highlight=(idx == cursor),
                        )
                    )
                self.write("\r\n".join(lines) + "\r\n")
                rendered = len(lines)
                key = self._read_key()
                if key in ("UP", "k", "K"):
                    cursor = (cursor - 1) % len(options)
                elif key in ("DOWN", "j", "J"):
                    cursor = (cursor + 1) % len(options)
                elif key == "ENTER":
                    return cursor
                elif key in ("CTRL_C", "q", "Q"):
                    raise UserCancelled("Installation canceled.")
        finally:
            termios.tcsetattr(self._fd, termios.TCSADRAIN, old_attrs)

    def select_many(self, title, options, defaults, *, min_selected=1, note=""):
        selected = list(defaults)
        if not self.can_raw:
            self.writeln(self.style.bold(title))
            if note:
                self.writeln(self.style.dim(f"  {note}"))
            while True:
                for idx, (label, desc) in enumerate(options, start=1):
                    box = "[x]" if selected[idx - 1] else "[ ]"
                    suffix = f" - {desc}" if desc else ""
                    self.writeln(f"  {idx}) {box} {label}{suffix}")
                raw = self.read_line(
                    "  Toggle numbers (space/comma), 'a' for all, or Enter to confirm: "
                )
                if not raw:
                    chosen = [i for i, flag in enumerate(selected) if flag]
                    if len(chosen) < min_selected:
                        self.writeln(
                            self.style.yellow(
                                f"  Select at least {min_selected} item(s)."
                            )
                        )
                        continue
                    return chosen
                if raw.lower() == "q":
                    raise UserCancelled("Installation canceled.")
                if raw.lower() == "a":
                    val = not all(selected)
                    selected = [val] * len(options)
                    continue
                tokens = [t for t in re.split(r"[\s,]+", raw) if t]
                if all(t.isdigit() and 1 <= int(t) <= len(options) for t in tokens):
                    for t in tokens:
                        selected[int(t) - 1] = not selected[int(t) - 1]
                else:
                    self.writeln(
                        self.style.yellow(
                            f"  Enter numbers 1-{len(options)} or press Enter."
                        )
                    )

        cursor = 0
        rendered = 0
        warning = ""
        self.writeln(self.style.bold(title))
        if note:
            self.writeln(self.style.dim(f"  {note}"))
        self.writeln(
            self.style.dim(
                "  Keys: Up/Down to move, Space to toggle, a for all, Enter to confirm, q to cancel"
            )
        )
        old_attrs = termios.tcgetattr(self._fd)
        try:
            tty.setraw(self._fd, when=termios.TCSADRAIN)
            while True:
                if rendered:
                    self.write(f"\033[{rendered}A")
                lines = []
                for idx, (label, desc) in enumerate(options):
                    pointer = self.style.cyan(">") if idx == cursor else " "
                    box = self.style.green("[x]") if selected[idx] else "[ ]"
                    lines.append(
                        self._format_option(
                            f"{pointer} {box}",
                            label,
                            desc,
                            highlight=(idx == cursor),
                        )
                    )
                footer = (
                    self.style.yellow(f"  {warning}")
                    if warning
                    else self.style.dim(f"  Selected {sum(selected)}/{len(options)}")
                )
                lines.append(f"\r\033[2K{footer}")
                self.write("\r\n".join(lines) + "\r\n")
                rendered = len(lines)
                key = self._read_key()
                warning = ""
                if key in ("UP", "k", "K"):
                    cursor = (cursor - 1) % len(options)
                elif key in ("DOWN", "j", "J"):
                    cursor = (cursor + 1) % len(options)
                elif key == "SPACE":
                    selected[cursor] = not selected[cursor]
                elif key in ("a", "A"):
                    val = not all(selected)
                    selected = [val] * len(options)
                elif key == "ENTER":
                    chosen = [i for i, flag in enumerate(selected) if flag]
                    if len(chosen) < min_selected:
                        warning = f"Select at least {min_selected} item(s)."
                        continue
                    return chosen
                elif key in ("CTRL_C", "q", "Q"):
                    raise UserCancelled("Installation canceled.")
        finally:
            termios.tcsetattr(self._fd, termios.TCSADRAIN, old_attrs)


def _redact_url_credentials(text):
    return re.sub(r"(https?://)[^/\s@]+@", r"\1<redacted>@", text)


def run(command, *, cwd=None, env=None, check=True):
    environment = {
        **(os.environ if env is None else env),
        "PYTHONDONTWRITEBYTECODE": "1",
        "GIT_TERMINAL_PROMPT": "0",
    }
    result = subprocess.run(
        [str(x) for x in command],
        cwd=cwd,
        env=environment,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
        timeout=COMMAND_TIMEOUT_SECONDS,
        check=False,
    )
    if check and result.returncode:
        detail = _redact_url_credentials((result.stderr or result.stdout).strip())
        raise ValueError(f"{Path(str(command[0])).name} failed: {detail[-2000:]}")
    return result


def run_with_progress(command, label, *, done_label=None, cwd=None, env=None):
    """Run a potentially slow command with a live TTY spinner and elapsed timer."""
    environment = {
        **(os.environ if env is None else env),
        "PYTHONDONTWRITEBYTECODE": "1",
        "GIT_TERMINAL_PROMPT": "0",
    }
    argv = [str(x) for x in command]
    interactive_out = color_enabled(sys.stdout)
    st = Style(interactive_out)
    frames = ("|", "/", "-", "\\")
    start = time.monotonic()

    if not interactive_out:
        print(f"{label}...", flush=True)
        result = subprocess.run(
            argv,
            cwd=cwd,
            env=environment,
            text=True,
            encoding="utf-8",
            errors="replace",
            capture_output=True,
            timeout=COMMAND_TIMEOUT_SECONDS,
            check=False,
        )
        elapsed = int(time.monotonic() - start)
        if result.returncode:
            detail = _redact_url_credentials((result.stderr or result.stdout).strip())
            raise ValueError(f"{Path(argv[0]).name} failed: {detail[-2000:]}")
        if done_label:
            print(f"{done_label} ({elapsed}s)", flush=True)
        return result.stdout

    proc = subprocess.Popen(
        argv,
        cwd=cwd,
        env=environment,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    stdout_text = ""
    stderr_text = ""
    frame_idx = 0
    try:
        while True:
            elapsed_float = time.monotonic() - start
            if elapsed_float > COMMAND_TIMEOUT_SECONDS:
                proc.kill()
                proc.wait()
                raise subprocess.TimeoutExpired(argv, COMMAND_TIMEOUT_SECONDS)
            try:
                stdout_text, stderr_text = proc.communicate(timeout=0.15)
                break
            except subprocess.TimeoutExpired:
                elapsed = int(time.monotonic() - start)
                spinner = st.cyan(frames[frame_idx % len(frames)])
                frame_idx += 1
                sys.stdout.write(
                    f"\r\033[2K  {spinner} {label}... {st.dim(f'({elapsed}s)')}"
                )
                sys.stdout.flush()
    except BaseException:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        sys.stdout.write("\r\033[2K")
        sys.stdout.flush()
        raise

    elapsed = int(time.monotonic() - start)
    sys.stdout.write("\r\033[2K")
    if proc.returncode:
        sys.stdout.flush()
        detail = _redact_url_credentials((stderr_text or stdout_text).strip())
        raise ValueError(f"{Path(argv[0]).name} failed: {detail[-2000:]}")
    if done_label:
        sys.stdout.write(f"  {st.green('ok')} {done_label} {st.dim(f'({elapsed}s)')}\n")
    sys.stdout.flush()
    return stdout_text


def unique_object(pairs):
    """Reject duplicate JSON keys rather than silently overwriting a field."""
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate dependency field: {key}")
        result[key] = value
    return result


def validate_lock_payload(data):
    if (
        not isinstance(data, dict)
        or data.get("schema") != "compile-debug-knowledge-dependencies-v1"
        or data.get("repository") != "https://gitcode.com/cann/cannbot-knowledge.git"
        or not re.fullmatch(r"[0-9a-f]{40}", str(data.get("commit", "")))
        or data.get("consumer_skills") != list(QUERIES)
        or data.get("skill_directory") != ".agents/skills"
        or data.get("python_minimum") != "3.10"
    ):
        raise ValueError("Invalid knowledge dependency lock")
    return data


def read_lock(path=None):
    if path is None:
        return validate_lock_payload(dict(PINNED_KNOWLEDGE_LOCK))
    raw = json.loads(
        Path(path).read_text(encoding="utf-8"), object_pairs_hook=unique_object
    )
    return validate_lock_payload(raw)


def parse_clients(raw):
    text = raw.strip()
    if not text:
        raise ValueError("Client list must not be empty.")
    if text.lower() == "all":
        return list(ALL_CLIENTS)
    items = [item.strip().lower() for item in text.split(",") if item.strip()]
    invalid = [item for item in items if item not in ALL_CLIENTS]
    if invalid:
        raise ValueError(
            f"Unsupported client(s): {', '.join(invalid)}. Choose from: {', '.join(ALL_CLIENTS)}, all."
        )
    ordered = []
    for item in items:
        if item not in ordered:
            ordered.append(item)
    return ordered


def claude_global_skills_dir(user_directory):
    config_dir = Path(
        os.environ.get("CLAUDE_CONFIG_DIR") or user_directory / ".claude"
    ).expanduser()
    if not config_dir.is_absolute():
        raise ValueError("CLAUDE_CONFIG_DIR must be an absolute path.")
    return config_dir.resolve() / "skills"


def resolve_destinations(scope, clients, project_root, custom_target, user_directory):
    if scope == "custom":
        target = custom_target.expanduser().resolve()
        return target, None
    use_shared = any(client in SHARED_CLIENTS for client in clients)
    use_claude = CLAUDE_CLIENT in clients
    if scope == "project":
        shared_dir = project_root / ".agents/skills"
        claude_dir = project_root / ".claude/skills"
    else:
        shared_dir = user_directory / ".agents/skills"
        claude_dir = claude_global_skills_dir(user_directory)
    if use_shared and use_claude:
        if claude_dir == shared_dir:
            return shared_dir, None
        return shared_dir, claude_dir
    if use_shared:
        return shared_dir, None
    return claude_dir, None


def config_values(path):
    if not path.is_file():
        return {}
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        tokens = shlex.split(stripped, comments=True)
        if tokens and tokens[0] == "export":
            tokens = tokens[1:]
        if len(tokens) == 1 and "=" in tokens[0]:
            key, value = tokens[0].split("=", 1)
            # Shell-style environment files take the last assignment.
            values[key] = value
    return values


def config_content(previous, knowledge, platform, *, relative_base=None):
    root_value = (
        os.path.relpath(knowledge.resolve(), relative_base.resolve())
        if relative_base is not None
        else str(knowledge)
    )
    updates = {ROOT_KEY: root_value}
    if platform:
        updates[PLATFORM_KEY] = platform
    lines = previous.decode("utf-8").splitlines() if previous else []
    kept = [
        line
        for line in lines
        if not any(re.match(rf"^\s*(?:export\s+)?{key}\s*=", line) for key in updates)
    ]
    return (
        "\n".join(
            [*kept, *(f"{key}={shlex.quote(value)}" for key, value in updates.items())]
        )
        + "\n"
    ).encode("utf-8")


def within_root(root, target):
    resolved_root = root.resolve()
    if not target.parent.resolve().is_relative_to(resolved_root):
        raise ValueError(
            f"Installation path escapes the root through a symlink: {target}"
        )
    for parent in (target.parent, *target.parent.parents):
        if parent == root or parent == resolved_root:
            break
        if parent.is_symlink():
            if not parent.resolve().is_relative_to(resolved_root):
                raise ValueError(
                    f"Installation path escapes the root through a symlink: {target}"
                )
        elif parent.exists() and not parent.is_dir():
            raise ValueError(f"Installation directory is occupied by a file: {parent}")


def validate_non_overlapping_roots(target_base, package, knowledge):
    paths = [p for p in (target_base, package, knowledge) if p is not None]
    if any(char in str(p) for p in paths for char in "\r\n"):
        raise ValueError("Installation paths must not contain line breaks.")
    if package is not None and target_base is not None:
        if target_base.resolve().is_relative_to(package.resolve()):
            raise ValueError(
                "Installation target must be outside the Skill source directory."
            )
    if knowledge is not None and target_base is not None:
        if target_base.resolve().is_relative_to(knowledge.resolve()):
            raise ValueError(
                "Installation target must be outside the knowledge repository."
            )
    if package is not None and knowledge is not None:
        if knowledge.resolve().is_relative_to(
            package.resolve()
        ) or package.resolve().is_relative_to(knowledge.resolve()):
            raise ValueError("Skill source and knowledge repository must not overlap.")


def _walk_no_symlinks(package):
    if package.is_symlink() or not package.is_dir():
        raise ValueError(
            f"Skill source must be a real directory, not a symlink: {package}"
        )
    for current_root, dirnames, filenames in os.walk(package, followlinks=False):
        current = Path(current_root)
        kept_dirs = []
        for dname in sorted(dirnames):
            if dname in COPY_IGNORE:
                continue
            dpath = current / dname
            if dpath.is_symlink():
                raise ValueError(
                    f"Managed Skill content must not contain symlinks: {dpath}"
                )
            kept_dirs.append(dname)
            yield dpath, True
        dirnames[:] = kept_dirs
        for fname in sorted(filenames):
            if fname in COPY_IGNORE or fname.endswith(".pyc"):
                continue
            fpath = current / fname
            if fpath.is_symlink():
                raise ValueError(
                    f"Managed Skill content must not contain symlinks: {fpath}"
                )
            if not fpath.is_file():
                raise ValueError(
                    f"Managed Skill content contains a non-regular file: {fpath}"
                )
            yield fpath, False


def validate_package(package):
    for _path, _is_dir in _walk_no_symlinks(package):
        pass
    entry = package / "SKILL.md"
    if not entry.is_file() or not re.search(
        rf"(?m)^name:\s*{SKILL}\s*$", entry.read_text(encoding="utf-8")
    ):
        raise ValueError(
            f"Package entry does not identify {SKILL}; pass the inner pure Skill directory."
        )
    names = {path.name for path in package.iterdir()} - COPY_IGNORE
    if names != PURE_TOP_LEVEL:
        raise ValueError(
            "Expected a pure tool-compile-debug Skill directory containing only SKILL.md, references, evals and scripts."
        )
    for rel in FORBIDDEN_PURE_PATHS:
        if (package / rel).exists() or (package / rel).is_symlink():
            raise ValueError(
                f"Pure Skill directory must not include installer or test artifact: {rel}"
            )
    for rel in REQUIRED_PURE_FILES:
        req = package / rel
        if req.is_symlink() or not req.is_file():
            raise ValueError(
                f"Incomplete tool-compile-debug runtime package (missing {rel})."
            )


def is_valid_installed_skill(package):
    """Verify whether an existing target directory is an installed pure tool-compile-debug copy."""
    if not package.is_dir() or package.is_symlink():
        return False
    try:
        for _path, _is_dir in _walk_no_symlinks(package):
            pass
        entry = package / "SKILL.md"
        if not entry.is_file() or not re.search(
            rf"(?m)^name:\s*{SKILL}\s*$", entry.read_text(encoding="utf-8")
        ):
            return False
        names = {path.name for path in package.iterdir()} - COPY_IGNORE
        return "SKILL.md" in names and names <= PURE_TOP_LEVEL
    except (OSError, UnicodeError, ValueError):
        return False


def package_digest(package):
    digest = hashlib.sha256()
    items = []
    for path, is_dir in _walk_no_symlinks(package):
        rel = path.relative_to(package).as_posix()
        items.append((rel, path, is_dir))
    items.sort(key=lambda item: item[0])
    for rel, path, is_dir in items:
        digest.update(rel.encode("utf-8") + b"\0")
        if is_dir:
            digest.update(b"D")
        else:
            digest.update(b"F" + hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def baseline_state_path(user_directory):
    return user_directory / ".cannbot" / ".compile-install-state.json"


def load_baseline_digest(user_directory, target):
    path = baseline_state_path(user_directory)
    if path.is_symlink() or path.parent.is_symlink() or not path.is_file():
        return None
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        installs = data.get("installs", {}) if isinstance(data, dict) else {}
        record = installs.get(str(target.resolve()))
        if isinstance(record, dict) and isinstance(record.get("digest"), str):
            return record["digest"]
    except (OSError, json.JSONDecodeError):
        return None
    return None


def save_baseline_digest(user_directory, target, digest):
    user_directory.mkdir(parents=True, exist_ok=True)
    path = baseline_state_path(user_directory)
    if path.parent.is_symlink() or path.is_symlink():
        raise ValueError(f"Baseline state path must not be a symlink: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    data = {"installs": {}}
    if path.is_file():
        try:
            loaded = json.loads(path.read_text(encoding="utf-8"))
            if isinstance(loaded, dict) and isinstance(loaded.get("installs"), dict):
                data = loaded
        except (OSError, json.JSONDecodeError):
            data = {"installs": {}}
    data["installs"][str(target.resolve())] = {"digest": digest}
    fd, tmp_name = tempfile.mkstemp(prefix=".compile-state-", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(data, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
        os.replace(tmp_name, path)
    finally:
        if os.path.exists(tmp_name):
            os.unlink(tmp_name)


def validate_repository_url(url):
    parsed = urlsplit(url)
    if parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise ValueError(
            "Repository URLs must not contain credentials or query parameters; use configured Git authentication."
        )


def fetch_pinned_checkout(destination, lock, *, offline):
    """Publish a new checkout at the exact pinned commit; never overwrite an existing directory."""
    validate_repository_url(lock["repository"])
    if destination.exists() or destination.is_symlink():
        raise FileExistsError(
            "Fetch destination must be new; use --knowledge-root to reuse an existing checkout."
        )
    if offline:
        raise ValueError(
            f"Offline installation cannot fetch a new repository: {destination}"
        )
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=".knowledge-fetch-", dir=destination.parent
    ) as tmp:
        stage = Path(tmp) / "checkout"
        run(["git", "init", stage])
        run(["git", "-C", stage, "remote", "add", "origin", lock["repository"]])
        run_with_progress(
            ["git", "-C", stage, "fetch", "--depth", "1", "origin", lock["commit"]],
            "Fetching pinned cannbot-knowledge",
            done_label="Fetched pinned cannbot-knowledge",
        )
        run(
            [
                "git",
                "-C",
                stage,
                "-c",
                "core.autocrlf=false",
                "checkout",
                "--detach",
                lock["commit"],
            ]
        )
        if destination.exists() or destination.is_symlink():
            raise FileExistsError("Fetch destination changed during preparation.")
        stage.rename(destination)


def verify_pinned_checkout(knowledge, lock):
    if knowledge.is_symlink() or not knowledge.is_dir():
        raise ValueError(
            f"Pinned knowledge root must be an existing directory: {knowledge}"
        )
    top = run(["git", "-C", knowledge, "rev-parse", "--show-toplevel"]).stdout.strip()
    if Path(top).resolve() != knowledge.resolve():
        raise ValueError("Knowledge root must be an independent complete checkout.")
    head = run(["git", "-C", knowledge, "rev-parse", "HEAD"]).stdout.strip()
    if head != lock["commit"]:
        raise ValueError("Knowledge checkout differs from the dependency lock.")
    if run(
        ["git", "-C", knowledge, "status", "--porcelain", "--untracked-files=no"]
    ).stdout.strip():
        raise ValueError("Knowledge checkout has modified tracked files.")
    return head


def repository(path, url, ref, *, offline, update):
    validate_repository_url(url)
    if path.is_symlink():
        raise ValueError(f"Managed repository must not be a symlink: {path}")
    if path.exists():
        if not path.is_dir():
            raise ValueError(f"Repository path is not a directory: {path}")
        if offline or not update:
            return
    elif offline:
        raise ValueError(f"Offline installation requires a local repository: {path}")
    run(["git", "check-ref-format", "--branch", ref])
    if path.exists():
        st = Style(color_enabled(sys.stdout))

        def warn_skip(reason):
            print(
                f"  {st.yellow('warn')} {path.name} {st.dim(f'(update skipped: {reason}; reusing existing checkout)')}",
                flush=True,
            )

        if not (path / ".git").exists():
            warn_skip("not a Git repository")
            return
        top = run(["git", "-C", path, "rev-parse", "--show-toplevel"]).stdout.strip()
        if Path(top).resolve() != path.resolve():
            warn_skip("not an independent Git repository")
            return
        origin = run(["git", "-C", path, "remote", "get-url", "origin"]).stdout.strip()
        if origin.removesuffix(".git").rstrip("/") != url.removesuffix(".git").rstrip(
            "/"
        ):
            warn_skip("origin URL differs")
            return
        branch = run(["git", "-C", path, "branch", "--show-current"]).stdout.strip()
        if branch != ref:
            warn_skip(f"current branch is {branch or 'detached HEAD'}, expected {ref}")
            return
        if run(
            ["git", "-C", path, "status", "--porcelain", "--untracked-files=no"]
        ).stdout.strip():
            warn_skip("local changes detected")
            return
        run_with_progress(
            ["git", "-C", path, "fetch", "origin", ref],
            f"Checking updates for {path.name}",
        )
        before = run(["git", "-C", path, "rev-parse", "HEAD"]).stdout.strip()
        ff_check = run(
            ["git", "-C", path, "merge-base", "--is-ancestor", before, "FETCH_HEAD"],
            check=False,
        )
        if ff_check.returncode != 0:
            warn_skip("cannot fast-forward")
            return
        run(["git", "-C", path, "merge", "--ff-only", "FETCH_HEAD"])
        after = run(["git", "-C", path, "rev-parse", "HEAD"]).stdout.strip()
        status = "updated to latest" if after != before else "already up to date"
        print(f"  {st.green('ok')} {path.name} {st.dim(f'({status})')}", flush=True)
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(
            prefix=".compile-clone-", dir=path.parent
        ) as tmp:
            staged = Path(tmp) / "checkout"
            run_with_progress(
                [
                    "git",
                    "clone",
                    "--depth",
                    "1",
                    "--single-branch",
                    "--branch",
                    ref,
                    "--",
                    url,
                    staged,
                ],
                f"Cloning {path.name}",
                done_label=f"Cloned {path.name}",
            )
            if path.exists() or path.is_symlink():
                raise ValueError(
                    "Destination changed during cloning; existing content was preserved."
                )
            staged.rename(path)


def _packages_ready(required):
    try:
        return all(
            low <= int(importlib.metadata.version(name).split(".")[0]) < high
            for name, low, high in required
        )
    except (importlib.metadata.PackageNotFoundError, ValueError):
        return False


def check_skill_runtime(*, offline, read_only):
    required = [("PyYAML", 6, 7)]
    if _packages_ready(required):
        return
    if offline or read_only:
        raise ValueError(
            "Selected Python is missing PyYAML (>=6,<7); install PyYAML in the selected Python environment."
        )
    run_with_progress(
        [sys.executable, "-m", "pip", "install", "PyYAML>=6,<7"],
        "Installing tool-compile-debug runtime dependency (PyYAML)",
        done_label="Installed PyYAML",
    )
    if not _packages_ready(required):
        raise ValueError(
            "PyYAML is still unavailable; check the selected Python environment."
        )


def check_knowledge_runtime(knowledge, *, offline, read_only):
    required = [("PyYAML", 6, 7)]
    if sys.version_info < (3, 11):
        required.append(("tomli", 2, 3))
    if _packages_ready(required):
        return
    if offline or read_only:
        raise ValueError(
            "Selected Python is missing dependencies; install the knowledge repository requirements.txt in the same environment."
        )
    run_with_progress(
        [sys.executable, "-m", "pip", "install", "-r", knowledge / "requirements.txt"],
        "Installing knowledge query dependencies",
        done_label="Installed Python dependencies",
    )
    if not _packages_ready(required):
        raise ValueError(
            "Runtime dependencies are still unavailable; check the selected Python environment."
        )


def validate_platform_registry(knowledge, platform):
    import yaml

    registry_path = knowledge / "governance/schemas/registries.yaml"
    try:
        registry = yaml.safe_load(registry_path.read_text(encoding="utf-8"))
    except yaml.YAMLError as exc:
        raise ValueError("Invalid knowledge platform registry YAML.") from exc
    if not isinstance(registry, dict) or not isinstance(
        registry.get("platforms"), dict
    ):
        raise ValueError("Invalid knowledge platform registry structure.")
    if platform is not None and platform not in registry["platforms"]:
        raise ValueError(
            "Platform must be an upstream platform ID registered in registries.yaml, not a raw SKU or architecture number."
        )


def prepare_knowledge(
    knowledge,
    *,
    platform,
    offline,
    read_only,
    with_sources=None,
    discover_cwd=None,
):
    resolved_root = knowledge.resolve()
    required = [
        "governance/schemas/registries.yaml",
        "requirements.txt",
        ".agents/skills/knowledge-query/scripts/knowledge_query.py",
        ".agents/skills/knowledge-query/scripts/knowledge_index.py",
        *[f".agents/skills/{name}/SKILL.md" for name in QUERIES],
    ]
    for rel in required:
        candidate = knowledge / rel
        if not candidate.is_file() or not candidate.resolve().is_relative_to(
            resolved_root
        ):
            raise ValueError(f"Incomplete or escaped knowledge resource: {rel}")

    check_knowledge_runtime(knowledge, offline=offline, read_only=read_only)
    validate_platform_registry(knowledge, platform)

    if with_sources:
        if offline or read_only:
            raise ValueError(
                "--with-sources cannot run in offline or read-only knowledge mode."
            )
        run_with_progress(
            ["bash", knowledge / "get_resources.sh", f"--{with_sources}"],
            f"Fetching source resources ({with_sources})",
            done_label=f"Source resources ready ({with_sources})",
            env={**os.environ, "CANNBOT_PYTHON": sys.executable},
        )

    index = knowledge / ".agents/skills/knowledge-query/scripts/knowledge_index.py"
    check = run(
        [
            sys.executable,
            index,
            "--knowledge-root",
            knowledge,
            "verify",
            "--level",
            "quick",
        ],
        check=False,
    )
    if check.returncode:
        if read_only:
            raise ValueError(
                "Knowledge index unavailable; prepare it before read-only installation."
            )
        run_with_progress(
            [sys.executable, index, "--knowledge-root", knowledge, "build"],
            "Building knowledge search index (~1 min on first run or after update)",
            done_label="Knowledge search index ready",
        )
        run(
            [
                sys.executable,
                index,
                "--knowledge-root",
                knowledge,
                "verify",
                "--level",
                "quick",
            ]
        )

    query = knowledge / ".agents/skills/knowledge-query/scripts/knowledge_query.py"
    run(
        [sys.executable, query, "--knowledge-root", knowledge, "discover"],
        cwd=discover_cwd,
    )


def write_config(path, expected_previous, knowledge, platform, *, relative_root):
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise ValueError(
            "Knowledge configuration path changed during preparation; configuration was not written."
        )
    current = path.read_bytes() if path.exists() else None
    if current != expected_previous:
        raise ValueError(
            "Configuration changed during preparation; original content was preserved."
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    relative_base = path.parent if relative_root else None
    content = config_content(
        expected_previous, knowledge.resolve(), platform, relative_base=relative_base
    )
    if expected_previous == content:
        return
    descriptor, temporary = tempfile.mkstemp(prefix=".knowledge-env-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
        if path.exists():
            os.chmod(temporary, path.stat().st_mode & 0o777)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def inspect_skill_conflict(
    root_guard, target, *, source, force, offline, baseline_digest
):
    """Return (action, conflict_path, reason). action in {'install', 'in_place', 'keep', 'replace'}."""
    if root_guard is not None:
        within_root(root_guard, target)
    if not target.exists() and not target.is_symlink():
        return "install", None, None
    if (
        source is not None
        and target.is_dir()
        and not target.is_symlink()
        and target.resolve() == source.resolve()
    ):
        return "in_place", None, None
    if is_valid_installed_skill(target):
        current_digest = package_digest(target)
        if source is not None and current_digest == package_digest(source):
            return "keep", None, None
        if source is None and offline:
            return "keep", None, None
        if baseline_digest is not None and current_digest == baseline_digest:
            return "replace", None, None
        if force:
            return "replace", None, None
        reason = (
            "local changes detected"
            if baseline_digest is not None
            else "unmanaged existing installation"
        )
        return "replace", target, reason
    if force:
        return "replace", None, None
    return "replace", target, "existing unmanaged path"


def inspect_link_conflict(root_guard, target, source, *, force):
    if root_guard is not None:
        within_root(root_guard, target)
    if not target.exists() and not target.is_symlink():
        return None
    if target.is_symlink():
        try:
            if target.resolve() == source.resolve():
                return None
        except OSError:
            pass
    if force:
        return None
    return target


class InstallTransaction:
    """Retain displaced directories, files, and symlinks until installation commits."""

    def __init__(self):
        self._backup_dirs: dict[Path, Path] = {}
        self._preserved_dirs: set[Path] = set()
        self._counter = 0
        self._records: list[tuple[Path, Path | None]] = []
        self._config_records: list[tuple[Path, bytes | None]] = []

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        for backup_dir in self._backup_dirs.values():
            if backup_dir in self._preserved_dirs:
                continue
            shutil.rmtree(backup_dir, ignore_errors=True)
        return False

    def _backup_slot(self, parent: Path) -> Path:
        resolved_parent = parent.resolve()
        backup_dir = self._backup_dirs.get(resolved_parent)
        if backup_dir is None:
            parent.mkdir(parents=True, exist_ok=True)
            backup_dir = Path(tempfile.mkdtemp(prefix=".compile-rollback-", dir=parent))
            self._backup_dirs[resolved_parent] = backup_dir
        self._counter += 1
        return backup_dir / f"entry-{self._counter}"

    def stash_existing(self, target: Path) -> Path | None:
        if not target.exists() and not target.is_symlink():
            return None
        slot = self._backup_slot(target.parent)
        target.rename(slot)
        return slot

    def record(self, target: Path, backup_slot: Path | None) -> None:
        self._records.append((target, backup_slot))

    def record_config(self, config_path: Path, previous_bytes: bytes | None) -> None:
        self._config_records.append((config_path, previous_bytes))

    def rollback(self) -> None:
        for config_path, previous_bytes in reversed(self._config_records):
            try:
                if previous_bytes is None:
                    config_path.unlink(missing_ok=True)
                else:
                    fd, tmp_cfg = tempfile.mkstemp(
                        prefix=".knowledge-env-rb-", dir=config_path.parent
                    )
                    try:
                        with os.fdopen(fd, "wb") as stream:
                            stream.write(previous_bytes)
                        os.replace(tmp_cfg, config_path)
                    finally:
                        if os.path.exists(tmp_cfg):
                            os.unlink(tmp_cfg)
            except OSError as exc:
                print(
                    f"Rollback warning: failed to restore {config_path}: {exc}",
                    file=sys.stderr,
                )

        for target, backup_slot in reversed(self._records):
            try:
                if target.is_dir() and not target.is_symlink():
                    shutil.rmtree(target)
                elif target.exists() or target.is_symlink():
                    target.unlink(missing_ok=True)
                if backup_slot is not None and (
                    backup_slot.exists() or backup_slot.is_symlink()
                ):
                    backup_slot.rename(target)
            except OSError as exc:
                if backup_slot is not None and (
                    backup_slot.exists() or backup_slot.is_symlink()
                ):
                    self._preserved_dirs.add(backup_slot.parent)
                    print(
                        f"Rollback warning: could not restore {target} ({exc}); backup preserved at {backup_slot}",
                        file=sys.stderr,
                    )


def copy_pure_skill(source, target, *, tx=None):
    validate_package(source)
    source_digest = package_digest(source)
    target.parent.mkdir(parents=True, exist_ok=True)

    def ignore(_directory, names):
        return {n for n in names if n in COPY_IGNORE or n.endswith(".pyc")}

    with contextlib.ExitStack() as local_stack:
        active_tx = (
            tx if tx is not None else local_stack.enter_context(InstallTransaction())
        )
        with tempfile.TemporaryDirectory(
            prefix=f".{SKILL}-stage-", dir=target.parent
        ) as tmp:
            staged = Path(tmp) / target.name
            shutil.copytree(source, staged, symlinks=True, ignore=ignore)
            for sh_file in staged.rglob("*.sh"):
                sh_file.chmod(0o755)
            validate_package(staged)
            digest = package_digest(staged)
            if digest != source_digest:
                raise ValueError("Staged Skill digest differed from verified source.")
            backup_slot = active_tx.stash_existing(target)
            try:
                staged.rename(target)
            except BaseException:
                if target.is_dir() and not target.is_symlink():
                    shutil.rmtree(target, ignore_errors=True)
                elif target.exists() or target.is_symlink():
                    target.unlink(missing_ok=True)
                if backup_slot is not None and (
                    backup_slot.exists() or backup_slot.is_symlink()
                ):
                    backup_slot.rename(target)
                raise
            active_tx.record(target, backup_slot)
            return digest


def create_symlink_atomic(target, source, *, relative, tx=None):
    """Create or update a directory symlink atomically."""
    target.parent.mkdir(parents=True, exist_ok=True)
    target_entry = target.parent.resolve() / target.name
    source_path = source.parent.resolve() / source.name
    if target_entry == source_path:
        raise ValueError(f"Refusing to create self-referential symlink: {target}")
    link_value = (
        os.path.relpath(source_path, target.parent.resolve())
        if relative
        else str(source_path)
    )
    if target.is_symlink():
        try:
            if (
                target.resolve() == source.resolve()
                and os.readlink(target) == link_value
            ):
                return "unchanged"
        except OSError:
            pass
    with contextlib.ExitStack() as local_stack:
        active_tx = (
            tx if tx is not None else local_stack.enter_context(InstallTransaction())
        )
        with tempfile.TemporaryDirectory(
            prefix=".skill-link-", dir=target.parent
        ) as tmp:
            staged = Path(tmp) / target.name
            staged.symlink_to(link_value, target_is_directory=True)
            backup_slot = active_tx.stash_existing(target)
            try:
                os.replace(staged, target)
            except BaseException:
                if (
                    backup_slot is not None
                    and not target.exists()
                    and not target.is_symlink()
                ):
                    backup_slot.rename(target)
                raise
            active_tx.record(target, backup_slot)
            return "replaced" if backup_slot is not None else "created"


def display_path(path, *, project_root=None, user_directory=None):
    resolved = path.expanduser().resolve()
    if project_root is not None:
        proj = project_root.expanduser().resolve()
        if resolved == proj:
            return "."
        if resolved.is_relative_to(proj):
            return "./" + resolved.relative_to(proj).as_posix()
    if user_directory is not None:
        home = user_directory.expanduser().resolve()
        if resolved == home:
            return "~"
        if resolved.is_relative_to(home):
            return "~/" + resolved.relative_to(home).as_posix()
    return str(resolved)


def prompt_interactive_choices(args, user_directory, session):
    """Collect target clients and install location interactively."""
    st = session.style
    cwd = Path.cwd().resolve()
    session.writeln()
    session.writeln(st.bold(f"{SKILL} installer"))
    session.writeln(st.dim(f"Installs pure {SKILL} and shared cannbot-knowledge."))
    session.writeln()

    client_groups = [
        ("codex / gemini / opencode", ".agents/skills", list(SHARED_CLIENTS)),
        ("claude", ".claude/skills", [CLAUDE_CLIENT]),
    ]
    chosen = session.select_many(
        "Step 1/2: Target client(s)",
        [(label, desc) for label, desc, _ in client_groups],
        defaults=[True, False],
        min_selected=1,
    )
    clients = []
    for idx in chosen:
        clients.extend(client_groups[idx][2])
    session.writeln()

    use_shared = any(c in SHARED_CLIENTS for c in clients)
    use_claude = CLAUDE_CLIENT in clients
    claude_global_label = display_path(
        claude_global_skills_dir(user_directory), user_directory=user_directory
    )

    if use_shared and use_claude:
        project_desc = "./.agents/skills + ./.claude/skills"
        global_desc = f"~/.agents/skills + {claude_global_label}"
        default_custom = cwd / ".agents/skills"
    elif use_shared:
        project_desc = "./.agents/skills"
        global_desc = "~/.agents/skills"
        default_custom = cwd / ".agents/skills"
    else:
        project_desc = "./.claude/skills"
        global_desc = claude_global_label
        default_custom = cwd / ".claude/skills"

    scope_idx = session.select_one(
        "Step 2/2: Install location",
        [
            ("Current project", project_desc),
            ("Global (all projects)", global_desc),
            ("Custom directory", "specify path"),
        ],
        default_index=0,
    )
    scope = ("project", "global", "custom")[scope_idx]
    project_arg = cwd if scope == "project" else None
    custom_target = None
    if scope == "custom":
        raw_target = session.prompt_text(
            "Target skills directory", default=str(default_custom)
        )
        custom_target = Path(raw_target)
        clients = []
    session.writeln()

    without_knowledge = args.without_knowledge
    knowledge_root = args.knowledge_root
    lock = read_lock()
    default_kb = (
        (user_directory / ".cannbot/pins" / lock["commit"][:12] / "cannbot-knowledge")
        if args.pinned
        else (user_directory / ".cannbot/cannbot-knowledge")
    ).resolve()

    project_preview = (
        project_arg.expanduser().resolve() if project_arg is not None else None
    )
    primary_dir, claude_dir = resolve_destinations(
        scope, clients, project_preview, custom_target, user_directory
    )

    def row(label, value):
        session.writeln(f"  {st.dim(f'{label:<12}')}  {value}")

    skill_dest = primary_dir / SKILL
    skill_tag = (
        st.dim(" (update)")
        if is_valid_installed_skill(skill_dest) and not args.offline
        else ""
    )
    session.writeln(st.bold("Summary"))
    row(
        "Skill",
        f"{display_path(skill_dest, project_root=project_preview, user_directory=user_directory)}{skill_tag}",
    )
    if claude_dir is not None:
        row(
            "Claude",
            f"{display_path(claude_dir, project_root=project_preview, user_directory=user_directory)} {st.dim('(symlink)')}",
        )
    if not without_knowledge:
        if args.fetch_root is not None:
            kb_preview = args.fetch_root.expanduser().resolve()
            kb_tag = st.dim(" (pinned fetch)")
        elif knowledge_root is not None:
            kb_preview = knowledge_root.expanduser().resolve()
            kb_tag = st.dim(" (pinned)" if args.pinned else "")
        elif os.environ.get(ROOT_KEY):
            kb_preview = Path(os.environ[ROOT_KEY]).expanduser().resolve()
            kb_tag = st.dim(f" (from {ROOT_KEY})")
        else:
            kb_preview = default_kb
            if args.pinned:
                kb_tag = st.dim(f" (pinned {lock['commit'][:12]})")
            else:
                action_note = (
                    ("reuse" if args.offline else "auto-update")
                    if kb_preview.is_dir()
                    else "clone"
                )
                kb_tag = st.dim(f" (shared across projects, {action_note})")
        row(
            "Knowledge",
            f"{display_path(kb_preview, project_root=project_preview, user_directory=user_directory)}{kb_tag}",
        )
        row("Queries", st.dim(", ".join(QUERIES)))
    else:
        row("Knowledge", st.dim("skipped"))
    session.writeln()

    if not session.prompt_confirm("Proceed?", default=True):
        raise UserCancelled("Installation canceled.")
    return (
        scope,
        clients,
        project_arg,
        custom_target,
        without_knowledge,
        knowledge_root,
    )


def build_parser():
    parser = argparse.ArgumentParser(
        prog="install.sh",
        description=f"Install {SKILL} and shared CANN knowledge queries into the current project, globally, or a custom directory.",
    )
    parser.add_argument("legacy_client", nargs="?", help=argparse.SUPPRESS)
    parser.add_argument("legacy_project", nargs="?", type=Path, help=argparse.SUPPRESS)
    parser.add_argument(
        "--clients",
        help="Target clients (comma-separated or 'all'): codex,gemini,opencode,claude (default: codex,gemini,opencode)",
    )
    parser.add_argument(
        "--project",
        type=Path,
        help="Existing project directory for project-level installs (default: current directory)",
    )
    parser.add_argument(
        "--global",
        dest="global_install",
        action="store_true",
        help="Install for the current user across projects",
    )
    parser.add_argument(
        "--target",
        type=Path,
        help="Install directly into a custom skills directory",
    )
    parser.add_argument(
        "--python",
        help="Python >= 3.10 interpreter (defaults to CANNBOT_PYTHON or auto-detected python3)",
    )
    parser.add_argument(
        "--package-root",
        type=Path,
        help=f"Reuse a local pure {SKILL} directory",
    )
    parser.add_argument(
        "--knowledge-root",
        type=Path,
        help="Reuse an existing local cannbot-knowledge repository (default: ~/.cannbot/cannbot-knowledge)",
    )
    parser.add_argument(
        "--fetch-root",
        type=Path,
        help="Fetch the pinned cannbot-knowledge commit into a NEW directory",
    )
    parser.add_argument(
        "--pinned",
        action="store_true",
        help="Use the pinned cannbot-knowledge commit for reproducible evaluation",
    )
    parser.add_argument(
        "--read-only-knowledge",
        action="store_true",
        help="Require an already prepared knowledge index without writing to the knowledge checkout",
    )
    parser.add_argument(
        "--platform",
        help="Optional default platform ID written to .cannbot/knowledge.env (project installs only)",
    )
    parser.add_argument(
        "--without-knowledge",
        action="store_true",
        help=f"Install only {SKILL} without cannbot-knowledge",
    )
    parser.add_argument(
        "--with-sources",
        choices=("docs", "code", "all"),
        help="Also fetch original documentation or operator sources via get_resources.sh",
    )
    parser.add_argument(
        "--offline",
        action="store_true",
        help="Use existing local files and Python dependencies only; skip remote updates",
    )
    parser.add_argument(
        "--update",
        action="store_true",
        help="Compatibility flag (online runs now update automatically by default)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite existing unmanaged files or modified Skill copies in the target directory",
    )
    parser.add_argument(
        "-y",
        "--yes",
        action="store_true",
        help="Non-interactive mode: accept defaults without prompting",
    )
    parser.add_argument(
        "--repository",
        default=PACKAGE_REPOSITORY,
        help=f"Git repository containing the {SKILL} release",
    )
    parser.add_argument(
        "--ref",
        default=PACKAGE_REF,
        help=f"Branch containing the {SKILL} release",
    )
    parser.add_argument(
        "--skill-path",
        default=f"skills/{SKILL}/skills/{SKILL}",
        help="Relative path of the pure Skill inside the repository",
    )
    parser.add_argument(
        "--knowledge-repository",
        default="https://gitcode.com/cann/cannbot-knowledge.git",
        help="cannbot-knowledge repository URL",
    )
    parser.add_argument(
        "--knowledge-ref",
        default=None,
        help="cannbot-knowledge branch (default: master)",
    )
    return parser


def main(argv=None, *, script_path=None):
    if argv is None:
        if sys.argv and sys.argv[0] == "-":
            script_path = sys.argv[1] if len(sys.argv) > 1 else ""
            argv = sys.argv[2:]
        else:
            script_path = sys.argv[0] if sys.argv else ""
            argv = sys.argv[1:]

    parser = build_parser()
    args = parser.parse_args(argv)
    if sys.version_info < (3, 10):
        parser.error("Python 3.10 or newer is required.")

    skill_path = PurePosixPath(args.skill_path)
    if (
        not skill_path.parts
        or skill_path.is_absolute()
        or ".." in skill_path.parts
        or re.search(r"[\x00-\x1f*?\[\]\\]", args.skill_path)
    ):
        parser.error(
            "--skill-path must be a relative repository path without wildcards."
        )

    if args.legacy_client is not None and args.clients is not None:
        parser.error("Do not combine a positional client argument with --clients.")
    if args.legacy_project is not None and args.project is not None:
        parser.error("Do not combine a positional project directory with --project.")
    if args.knowledge_root is not None and args.fetch_root is not None:
        parser.error("Do not combine --knowledge-root with --fetch-root.")
    if args.offline and (
        args.update or args.with_sources or args.fetch_root is not None
    ):
        parser.error(
            "--offline cannot be combined with --update, --with-sources, or --fetch-root."
        )
    if args.read_only_knowledge and (
        args.update or args.with_sources or args.fetch_root is not None
    ):
        parser.error(
            "--read-only-knowledge cannot be combined with --update, --with-sources, or --fetch-root."
        )

    pinned_mode = bool(args.pinned or args.fetch_root is not None)
    if pinned_mode and args.knowledge_ref is not None:
        parser.error("Do not combine --pinned/--fetch-root with --knowledge-ref.")
    knowledge_ref = args.knowledge_ref or "master"

    if args.without_knowledge and (
        args.with_sources
        or args.platform
        or args.knowledge_root is not None
        or args.fetch_root is not None
        or args.pinned
        or args.read_only_knowledge
    ):
        parser.error(
            "--without-knowledge cannot be combined with knowledge or platform options."
        )

    project_arg = args.project if args.project is not None else args.legacy_project
    if args.global_install and project_arg is not None:
        parser.error(
            "--global cannot be combined with a project directory; omit the directory argument."
        )
    if args.global_install and args.target is not None:
        parser.error("--global cannot be combined with --target.")
    if args.global_install and args.platform:
        parser.error(
            "--platform is a project setting; select the target platform when querying a global installation."
        )
    if args.target is not None and (
        project_arg is not None
        or args.clients is not None
        or args.legacy_client is not None
        or args.platform
    ):
        parser.error(
            "--target cannot be combined with --project, --clients, --platform, or positional arguments."
        )

    explicit_non_interactive = args.yes or any(
        value is not None and value is not False
        for value in (
            args.clients,
            args.global_install,
            args.project,
            args.target,
            args.legacy_client,
            args.legacy_project,
            args.fetch_root,
        )
    )

    user_directory = Path.home().expanduser().resolve(strict=args.global_install)
    without_knowledge = args.without_knowledge
    knowledge_root_arg = args.knowledge_root
    custom_target = args.target

    session = TTYSession()
    opened_tty = False
    try:
        if not explicit_non_interactive:
            opened_tty = session.open()
        if opened_tty:
            (
                scope,
                clients,
                project_arg,
                custom_target,
                without_knowledge,
                knowledge_root_arg,
            ) = prompt_interactive_choices(args, user_directory, session)
        else:
            scope = (
                "custom"
                if custom_target is not None
                else ("global" if args.global_install else "project")
            )
            raw_clients = (
                args.clients
                if args.clients is not None
                else (
                    args.legacy_client
                    if args.legacy_client is not None
                    else ",".join(SHARED_CLIENTS)
                )
            )
            clients = [] if scope == "custom" else parse_clients(raw_clients)

        if scope != "project" and args.platform:
            raise ValueError(
                "--platform is a project setting; select a project installation or omit --platform."
            )

        if scope == "global":
            user_directory = Path.home().expanduser().resolve(strict=True)

        project_root = None
        if scope == "project":
            raw_project = (project_arg or Path.cwd()).expanduser()
            if not raw_project.exists() or not raw_project.is_dir():
                raise ValueError(
                    "Installation project root must be an existing directory."
                )
            project_root = raw_project.resolve(strict=True)

        primary_dir, claude_dir = resolve_destinations(
            scope, clients, project_root, custom_target, user_directory
        )
        primary_guard = (
            project_root
            if scope == "project"
            else (primary_dir.parent if scope == "global" else None)
        )
        claude_guard = (
            project_root
            if scope == "project"
            else (
                claude_dir.parent
                if (scope == "global" and claude_dir is not None)
                else None
            )
        )
        if claude_dir is not None:
            if claude_guard is not None:
                within_root(claude_guard, claude_dir / SKILL)
            if claude_dir.resolve() == primary_dir.resolve():
                claude_dir = None
                claude_guard = None

        config_path = None
        existing_config = {}
        expected_config_bytes = None
        if not without_knowledge:
            if scope == "project":
                config_path = project_root / ".cannbot/knowledge.env"
                within_root(project_root, config_path)
            elif scope == "global":
                config_path = user_directory / ".cannbot/knowledge.env"
            if config_path is not None:
                if config_path.is_symlink() or (
                    config_path.exists() and not config_path.is_file()
                ):
                    raise ValueError(
                        "Knowledge configuration path is a symlink or directory; it was not overwritten."
                    )
                if config_path.exists():
                    expected_config_bytes = config_path.read_bytes()
                    existing_config = config_values(config_path)

        # Resolve pure Skill source (explicit --package-root / adjacent inner directory / remote sparse checkout).
        local_package = None
        if args.package_root is not None:
            raw_pkg = args.package_root.expanduser()
            if raw_pkg.is_symlink():
                raise ValueError(
                    f"Skill source must be a real directory, not a symlink: {raw_pkg}"
                )
            local_package = raw_pkg.resolve()
        elif script_path and Path(script_path).is_file():
            adjacent = Path(script_path).resolve().parent.parent / "skills" / SKILL
            if (adjacent / "SKILL.md").is_file() and not adjacent.is_symlink():
                local_package = adjacent
        if local_package is not None:
            validate_package(local_package)

        lock = read_lock() if pinned_mode else None
        shared_default_knowledge = (
            user_directory / ".cannbot/cannbot-knowledge"
        ).resolve()
        pinned_default_knowledge = (
            (
                user_directory
                / ".cannbot/pins"
                / lock["commit"][:12]
                / "cannbot-knowledge"
            ).resolve()
            if lock is not None
            else None
        )
        knowledge = None
        managed_knowledge = False
        using_shared_default = False
        effective_platform = args.platform
        if not without_knowledge:
            if effective_platform is None and existing_config.get(PLATFORM_KEY):
                effective_platform = existing_config[PLATFORM_KEY]
            env_root = os.environ.get(ROOT_KEY)
            if args.fetch_root is not None:
                raw_fetch = args.fetch_root.expanduser()
                if raw_fetch.exists() or raw_fetch.is_symlink():
                    raise FileExistsError(
                        "Fetch destination must be new; use --knowledge-root to reuse an existing checkout."
                    )
                knowledge = raw_fetch.resolve()
            elif knowledge_root_arg is not None:
                raw_kb = knowledge_root_arg.expanduser()
                if raw_kb.is_symlink():
                    raise ValueError(f"Knowledge root must not be a symlink: {raw_kb}")
                knowledge = raw_kb.resolve()
            elif pinned_mode:
                knowledge = pinned_default_knowledge
            elif existing_config.get(ROOT_KEY) and config_path is not None:
                raw_val = Path(existing_config[ROOT_KEY]).expanduser()
                knowledge = (
                    raw_val if raw_val.is_absolute() else config_path.parent / raw_val
                ).resolve()
            elif env_root:
                knowledge = Path(env_root).expanduser().resolve()
            else:
                knowledge = shared_default_knowledge
                using_shared_default = True
            if not pinned_mode:
                managed_knowledge = knowledge == shared_default_knowledge or (
                    config_path is not None
                    and knowledge
                    == (config_path.parent / "cannbot-knowledge").resolve()
                )
            if (
                not pinned_mode
                and knowledge_root_arg is None
                and env_root
                and Path(env_root).expanduser().resolve() != knowledge
            ):
                raise ValueError(
                    "CANNBOT_KNOWLEDGE_ROOT conflicts with the existing knowledge configuration; unset it in this shell and retry."
                )

        target_base = project_root if scope == "project" else primary_dir
        validate_non_overlapping_roots(target_base, local_package, knowledge)

        # Preflight conflict checks before any network or filesystem mutations.
        primary_skill_target = primary_dir / SKILL
        baseline_digest = load_baseline_digest(user_directory, primary_skill_target)
        conflicts = []
        skill_action, skill_conflict, skill_conflict_reason = inspect_skill_conflict(
            primary_guard,
            primary_skill_target,
            source=local_package,
            force=args.force,
            offline=args.offline,
            baseline_digest=baseline_digest,
        )
        if skill_conflict is not None:
            conflicts.append((skill_conflict, skill_conflict_reason))

        primary_query_links = []
        if knowledge is not None:
            primary_query_links = [
                (primary_dir / name, knowledge / ".agents/skills" / name)
                for name in QUERIES
            ]
            for target, source in primary_query_links:
                conflict = inspect_link_conflict(
                    primary_guard, target, source, force=args.force
                )
                if conflict is not None:
                    conflicts.append((conflict, "existing path or unrelated symlink"))

        claude_links = []
        if claude_dir is not None:
            claude_names = [SKILL, *(QUERIES if knowledge is not None else ())]
            for name in claude_names:
                target = claude_dir / name
                source = primary_dir / name
                conflict = inspect_link_conflict(
                    claude_guard, target, source, force=args.force
                )
                if conflict is not None:
                    conflicts.append((conflict, "existing path or unrelated symlink"))
                claude_links.append((target, source))

        if conflicts:
            if opened_tty:
                session.writeln()
                session.writeln(
                    session.style.yellow("Existing entries found in target directory:")
                )
                for path, reason in conflicts:
                    disp = display_path(
                        path,
                        project_root=project_root,
                        user_directory=user_directory,
                    )
                    session.writeln(f"  - {disp} ({reason})")
                if not session.prompt_confirm(
                    "Overwrite existing entries?", default=False
                ):
                    raise UserCancelled(
                        "Installation canceled; existing files were preserved."
                    )
            else:
                listed = ", ".join(f"{p} ({reason})" for p, reason in conflicts)
                raise FileExistsError(
                    f"Existing path conflicts with this installation; original content was preserved (pass --force to overwrite): {listed}"
                )
    finally:
        if opened_tty:
            session.close()

    need_remote_skill = local_package is None and skill_action in ("install", "replace")
    if need_remote_skill and args.offline:
        raise ValueError(
            f"Offline installation requires an existing local Skill or --package-root: {primary_skill_target}"
        )
    need_git_for_knowledge = False
    if knowledge is not None:
        if pinned_mode:
            need_git_for_knowledge = True
        elif managed_knowledge and (not args.offline and not args.read_only_knowledge):
            need_git_for_knowledge = True
        elif managed_knowledge and not knowledge.exists():
            need_git_for_knowledge = True
    if (need_remote_skill or need_git_for_knowledge) and not shutil.which("git"):
        raise ValueError("Git is required to fetch or verify repositories.")

    if scope == "project":
        guard_dir = project_root
    elif scope == "global":
        guard_dir = user_directory / ".cannbot"
    else:
        guard_dir = primary_dir
    guard_dir.mkdir(parents=True, exist_ok=True)
    guard = guard_dir / ".compile-debug-install.lock"
    with guard.open("x", encoding="utf-8") as lock_stream:
        lock_stream.write(str(os.getpid()))

    installed_digest = None
    with InstallTransaction() as tx:
        try:
            # Step 1: Prepare cannbot-knowledge (or check standalone PyYAML runtime when --without-knowledge).
            if knowledge is None:
                check_skill_runtime(
                    offline=args.offline, read_only=args.read_only_knowledge
                )
            else:
                if pinned_mode:
                    if args.fetch_root is not None or (
                        knowledge == pinned_default_knowledge and not knowledge.exists()
                    ):
                        if args.read_only_knowledge:
                            raise ValueError(
                                "Read-only knowledge installation requires an existing prepared checkout."
                            )
                        fetch_pinned_checkout(knowledge, lock, offline=args.offline)
                    verify_pinned_checkout(knowledge, lock)
                elif managed_knowledge:
                    if args.read_only_knowledge and not knowledge.exists():
                        raise ValueError(
                            f"Read-only knowledge installation requires an existing local repository: {knowledge}"
                        )
                    repository(
                        knowledge,
                        args.knowledge_repository,
                        knowledge_ref,
                        offline=(args.offline or args.read_only_knowledge),
                        update=(not args.offline and not args.read_only_knowledge),
                    )
                prepare_knowledge(
                    knowledge,
                    platform=effective_platform,
                    offline=args.offline,
                    read_only=args.read_only_knowledge,
                    with_sources=args.with_sources,
                    discover_cwd=project_root or user_directory,
                )

            # Step 2: Fetch (if remote) and install pure tool-compile-debug into primary_dir.
            if skill_action in ("in_place", "keep"):
                pass
            elif local_package is not None:
                installed_digest = copy_pure_skill(
                    local_package, primary_skill_target, tx=tx
                )
            elif need_remote_skill:
                validate_repository_url(args.repository)
                run(["git", "check-ref-format", "--branch", args.ref])
                with tempfile.TemporaryDirectory(prefix=f".{SKILL}-fetch-") as tmp:
                    checkout = Path(tmp) / "checkout"
                    action_word = (
                        "Updating" if primary_skill_target.exists() else "Fetching"
                    )
                    run_with_progress(
                        [
                            "git",
                            "clone",
                            "--depth",
                            "1",
                            "--single-branch",
                            "--branch",
                            args.ref,
                            "--filter=blob:none",
                            "--no-checkout",
                            "--",
                            args.repository,
                            checkout,
                        ],
                        f"{action_word} {SKILL}",
                    )
                    run(
                        [
                            "git",
                            "-C",
                            checkout,
                            "sparse-checkout",
                            "set",
                            "--no-cone",
                            f"/{skill_path.as_posix()}/",
                        ]
                    )
                    run(
                        [
                            "git",
                            "-C",
                            checkout,
                            "-c",
                            "core.autocrlf=false",
                            "checkout",
                            args.ref,
                        ]
                    )
                    downloaded = checkout / str(skill_path)
                    if (
                        downloaded.is_symlink()
                        or not downloaded.resolve().is_relative_to(checkout.resolve())
                    ):
                        raise ValueError(
                            "Downloaded Skill path escapes the repository or is a symlink."
                        )
                    validate_package(downloaded)
                    installed_digest = copy_pure_skill(
                        downloaded, primary_skill_target, tx=tx
                    )

            # Step 3: Symlink the 2 knowledge query skills into primary_dir.
            for target, source in primary_query_links:
                use_rel = (
                    scope == "project"
                    and knowledge is not None
                    and knowledge.is_relative_to(project_root)
                )
                create_symlink_atomic(target, source, relative=use_rel, tx=tx)

            # Step 4: Create Claude Code adapter symlinks when both .agents/skills and claude are selected.
            if claude_dir is not None:
                use_rel = scope == "project" or (
                    claude_dir.parent.parent == primary_dir.parent.parent
                )
                for target, source in claude_links:
                    create_symlink_atomic(target, source, relative=use_rel, tx=tx)

            # Step 5: Write .cannbot/knowledge.env (project or global).
            if knowledge is not None and config_path is not None:
                if scope == "project":
                    within_root(project_root, config_path)
                rel_root = scope == "project" and knowledge.is_relative_to(project_root)
                write_config(
                    config_path,
                    expected_config_bytes,
                    knowledge,
                    args.platform if args.platform is not None else effective_platform,
                    relative_root=rel_root,
                )
                tx.record_config(config_path, expected_config_bytes)

            # Step 6: Persist baseline digest inside transaction before committing backups.
            if installed_digest is not None:
                save_baseline_digest(
                    user_directory, primary_skill_target, installed_digest
                )
        except BaseException:
            tx.rollback()
            raise
        finally:
            guard.unlink(missing_ok=True)

    st = Style(color_enabled(sys.stdout))

    def out_row(label, value):
        print(f"  {st.dim(f'{label:<10}')}  {value}")

    print()
    print(st.green(st.bold("Installed successfully!")))
    out_row(
        "Skill",
        display_path(
            primary_skill_target,
            project_root=project_root,
            user_directory=user_directory,
        ),
    )
    if claude_dir is not None:
        out_row(
            "Claude",
            f"{display_path(claude_dir, project_root=project_root, user_directory=user_directory)} {st.dim('(symlink)')}",
        )
    if knowledge is not None:
        shared_tag = (
            st.dim(" (shared across projects)")
            if using_shared_default
            else (st.dim(f" (pinned {lock['commit'][:12]})") if pinned_mode else "")
        )
        out_row(
            "Knowledge",
            f"{display_path(knowledge, project_root=project_root, user_directory=user_directory)}{shared_tag}",
        )
        if config_path is not None:
            out_row(
                "Config",
                display_path(
                    config_path,
                    project_root=project_root,
                    user_directory=user_directory,
                ),
            )
    out_row("Python", sys.executable)
    print()
    print(st.dim(f"Restart your client to load {SKILL}."))
    return 0


def cli_entry(argv=None, *, script_path=None):
    try:
        return main(argv, script_path=script_path)
    except UserCancelled as exc:
        print(f"\n{exc}", file=sys.stderr)
        return 130
    except (
        OSError,
        ValueError,
        ImportError,
        subprocess.SubprocessError,
    ) as exc:
        print(f"Installation failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(cli_entry())
PY
