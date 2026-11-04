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

# Install the precision Skill and optional CANN knowledge consumers locally or globally.
# This file is self-contained so it can also be downloaded and executed directly.
set -euo pipefail
python_bin="${CANNBOT_PYTHON:-python3}"
python_next=false
for argument in "$@"; do
  if [[ "$python_next" == true ]]; then
    python_bin="$argument"
    python_next=false
  elif [[ "$argument" == --python ]]; then
    python_next=true
  elif [[ "$argument" == --python=* ]]; then
    python_bin="${argument#--python=}"
  fi
done
command -v "$python_bin" >/dev/null 2>&1 || { echo 'Python 3.10 or newer is required; select it with --python.' >&2; exit 2; }
exec "$python_bin" - "${BASH_SOURCE[0]:-}" "$@" <<'PY'
from __future__ import annotations

import argparse
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
from urllib.parse import urlsplit

SKILL = "tool-precision-debug"
PACKAGE_REPOSITORY = "https://gitcode.com/cann/asc-tools.git"
PACKAGE_REF = "master"
PACKAGE_DIRECTORY = "asc-tools"
QUERIES = ("knowledge-query", "ascendc-api-knowledge-query")
# OpenCode discovers .agents/skills directly; only Claude needs a client alias.
ADAPTERS = {"codex": None, "claude": ".claude/skills", "opencode": None}
ROOT_KEY = "CANNBOT_KNOWLEDGE_ROOT"
PLATFORM_KEY = "CANNBOT_KNOWLEDGE_PLATFORM"


def global_skill_directory(client, user_directory):
    if client == "codex":
        config_directory = user_directory / ".agents"
    elif client == "claude":
        config_directory = Path(os.environ.get("CLAUDE_CONFIG_DIR") or user_directory / ".claude")
    else:
        xdg_directory = Path(os.environ.get("XDG_CONFIG_HOME") or user_directory / ".config")
        config_directory = Path(os.environ.get("OPENCODE_CONFIG_DIR") or xdg_directory / "opencode")
    config_directory = config_directory.expanduser()
    if not config_directory.is_absolute():
        raise ValueError("Global client configuration directory must be an absolute path.")
    return config_directory.resolve() / "skills"


def run(command, *, cwd=None, env=None):
    environment = {**(os.environ if env is None else env), "PYTHONDONTWRITEBYTECODE": "1"}
    result = subprocess.run(command, cwd=cwd, env=environment, text=True, capture_output=True)
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        detail = re.sub(r"(https?://)[^/\s@]+@", r"\1<redacted>@", detail)
        raise ValueError(f"{Path(command[0]).name} failed: {detail[-2000:]}")
    return result.stdout


def config_values(path):
    if not path.is_file():
        return {}
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        tokens = shlex.split(line, comments=True)
        if tokens and tokens[0] == "export":
            tokens = tokens[1:]
        if len(tokens) == 1 and "=" in tokens[0]:
            key, value = tokens[0].split("=", 1)
            values.setdefault(key, value)
    return values


def within_project(project, target):
    if not target.parent.resolve().is_relative_to(project):
        raise ValueError(f"Installation path escapes the project through a symlink: {target}")
    for parent in (target.parent, *target.parent.parents):
        if parent == project:
            break
        if parent.exists() and not parent.is_dir():
            raise ValueError(f"Installation directory is occupied by a file: {parent}")


def preflight_link(project, target, source, replace_sources=()):
    within_project(project, target)
    # Aggregating installers may already have copied the pure Skill here.
    if target.is_dir() and target.resolve() == source.resolve():
        return
    if target.is_symlink() and target.resolve() == source.resolve():
        return
    if target.is_symlink() and target.resolve() in replace_sources:
        return
    if target.exists() or target.is_symlink():
        raise ValueError(f"Existing path conflicts with this installation; original content was preserved: {target}")


def repository(path, url, ref, *, offline, update, sparse=None, quiet=False):
    parsed = urlsplit(url)
    if parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise ValueError("Repository URLs must not contain credentials or query parameters; use configured Git authentication.")
    run(["git", "check-ref-format", "--branch", ref])
    if path.is_symlink():
        raise ValueError(f"Managed repository must not be a symlink: {path}")
    if path.exists():
        if not path.is_dir():
            raise ValueError(f"Repository path is not a directory: {path}")
        top = run(["git", "-C", str(path), "rev-parse", "--show-toplevel"]).strip()
        if Path(top).resolve() != path.resolve():
            raise ValueError("Managed path is not an independent Git repository; its parent repository was not used.")
        origin = run(["git", "-C", str(path), "remote", "get-url", "origin"]).strip()
        if origin.removesuffix(".git").rstrip("/") != url.removesuffix(".git").rstrip("/"):
            raise ValueError("Repository origin does not match; the existing repository was not updated.")
        branch = run(["git", "-C", str(path), "branch", "--show-current"]).strip()
        if branch != ref:
            raise ValueError("Current branch does not match the requested branch; no branch was switched.")
        if run(["git", "-C", str(path), "status", "--porcelain", "--untracked-files=no"]).strip():
            if update or sparse:
                raise ValueError("Repository has local changes; update and sparse checkout were skipped.")
        if sparse:
            run(["git", "-C", str(path), "sparse-checkout", "set", "--no-cone", f"/{sparse}/"])
        if update:
            if not quiet:
                print(f"Updating {path.name}...", flush=True)
            run(["git", "-C", str(path), "fetch", "origin", ref])
            run(["git", "-C", str(path), "merge", "--ff-only", "FETCH_HEAD"])
    else:
        if offline:
            raise ValueError(f"Offline installation requires a local repository: {path}")
        path.parent.mkdir(parents=True, exist_ok=True)
        if not quiet:
            print(f"Fetching {path.name}...", flush=True)
        with tempfile.TemporaryDirectory(prefix=".precision-clone-", dir=path.parent) as tmp:
            staged = Path(tmp) / "checkout"
            command = ["git", "clone", "--depth", "1", "--single-branch", "--branch", ref]
            if sparse:
                command += ["--filter=blob:none", "--no-checkout"]
            run([*command, "--", url, str(staged)])
            if sparse:
                run(["git", "-C", str(staged), "sparse-checkout", "set", "--no-cone", f"/{sparse}/"])
                run(["git", "-C", str(staged), "checkout", ref])
            if path.exists() or path.is_symlink():
                raise ValueError("Destination changed during cloning; existing content was preserved.")
            staged.rename(path)


def validate_package(package):
    entry = package / "SKILL.md"
    if not entry.is_file() or not re.search(rf"(?m)^name:\s*{SKILL}\s*$", entry.read_text(encoding="utf-8")):
        raise ValueError("No tool-precision-debug release found; build development sources with scripts/install-dev.sh first.")
    if {path.name for path in package.iterdir()} != {"SKILL.md", "references", "evals"} or list(package.rglob("viz.html")):
        raise ValueError("Expected a pure Skill directory containing only SKILL.md, references and evals.")


def package_digest(package):
    digest = hashlib.sha256()
    for path in sorted(package.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"Managed Skill content must not contain symlinks: {path}")
        digest.update(path.relative_to(package).as_posix().encode() + b"\0")
        digest.update(b"F" + hashlib.sha256(path.read_bytes()).digest() if path.is_file() else b"D")
    return digest.hexdigest()


def install_package(project, package, url, ref, skill_path, *, offline, update):
    """Keep only the pure Skill; use a temporary checkout for downloads and updates."""
    state = package.parent / f".{SKILL}.json"
    within_project(project, package)
    within_project(project, state)
    if package.is_symlink() or state.is_symlink():
        raise ValueError("Managed Skill and installation state must not be symlinks.")
    requested = {"repository": url, "ref": ref, "skill_path": skill_path.as_posix()}
    previous_state = None
    if package.exists() or state.exists():
        if not package.is_dir() or not state.is_file():
            raise ValueError("Existing Skill has no matching installation state; existing files were preserved.")
        previous_state = state.read_bytes()
        recorded = json.loads(previous_state)
        if not isinstance(recorded, dict) or any(recorded.get(key) != value for key, value in requested.items()):
            raise ValueError("Skill source does not match this installation; existing files were preserved.")
        validate_package(package)
        if recorded.get("digest") != package_digest(package):
            raise ValueError("Managed Skill contains local changes; existing files were preserved.")
        if not update:
            return
    elif offline:
        raise ValueError(f"Offline installation requires an existing Skill: {package}")
    package.parent.mkdir(parents=True, exist_ok=True)
    print(f"{'Updating' if package.exists() else 'Fetching'} {SKILL}...", flush=True)
    # Stage beside the destination so replacing the directory stays on one filesystem.
    with tempfile.TemporaryDirectory(prefix=f".{SKILL}-", dir=package.parent) as temporary:
        staging = Path(temporary)
        checkout = staging / "checkout"
        repository(checkout, url, ref, offline=False, update=False, sparse=skill_path.as_posix(), quiet=True)
        downloaded = checkout / str(skill_path)
        if downloaded.is_symlink() or not downloaded.resolve().is_relative_to(checkout.resolve()):
            raise ValueError("Downloaded Skill path escapes the repository or is a symlink.")
        validate_package(downloaded)
        staged_state = staging / "state.json"
        staged_state.write_text(json.dumps({**requested, "digest": package_digest(downloaded)}) + "\n", encoding="utf-8")
        within_project(project, package)
        if package.is_symlink() or state.is_symlink():
            raise ValueError("Installation paths changed during download; existing files were preserved.")
        if previous_state is None:
            if package.exists() or state.exists():
                raise ValueError("Installation destination appeared during download; existing files were preserved.")
        elif (not package.is_dir() or not state.is_file() or state.read_bytes() != previous_state
              or package_digest(package) != recorded["digest"]):
            raise ValueError("Installed Skill changed during download; existing files were preserved.")
        backup = staging / "previous"
        if package.exists():
            package.rename(backup)
        try:
            downloaded.rename(package)
            os.replace(staged_state, state)
        except BaseException:
            if package.exists():
                shutil.rmtree(package)
            if backup.exists():
                backup.rename(package)
            raise


def check_runtime(knowledge, offline):
    def ready():
        required = [("PyYAML", 6, 7)]
        if sys.version_info < (3, 11):
            required.append(("tomli", 2, 3))
        try:
            return all(low <= int(importlib.metadata.version(name).split(".")[0]) < high for name, low, high in required)
        except (importlib.metadata.PackageNotFoundError, ValueError):
            return False

    if ready():
        return
    if offline:
        raise ValueError("Selected Python is missing dependencies; install the knowledge repository requirements.txt in the same environment.")
    print("Installing knowledge query dependencies...", flush=True)
    run([sys.executable, "-m", "pip", "install", "-r", str(knowledge / "requirements.txt")])
    if not ready():
        raise ValueError("Runtime dependencies are still unavailable; check the selected Python environment.")


def write_config(path, knowledge, platform):
    previous = path.read_bytes() if path.exists() else None
    path.parent.mkdir(parents=True, exist_ok=True)
    updates = {ROOT_KEY: os.path.relpath(knowledge.resolve(), path.parent.resolve())}
    if platform:
        updates[PLATFORM_KEY] = platform
    lines = (previous.decode("utf-8").splitlines() if previous is not None else [])
    kept = [line for line in lines if not any(re.match(rf"^\s*(?:export\s+)?{key}=", line) for key in updates)]
    content = "\n".join([*kept, *(f"{key}={shlex.quote(value)}" for key, value in updates.items())]) + "\n"
    if previous == content.encode():
        return
    descriptor, temporary = tempfile.mkstemp(prefix=".knowledge-env-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(content)
        if path.exists():
            os.chmod(temporary, path.stat().st_mode & 0o777)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def remove_opencode_aliases(project, canonical):
    """Remove redundant links from earlier installs, preserving unrelated entries."""
    removed = False
    for target, _ in canonical:
        alias = project / ".opencode/skills" / target.name
        if alias.parent.resolve() == target.parent.resolve():
            continue  # A shared parent must not turn alias cleanup into Skill deletion.
        if not alias.is_symlink() or alias.resolve() != target.resolve():
            continue
        try:
            within_project(project, alias)
            alias.unlink()
        except (OSError, ValueError):
            print(f"Could not remove the old client link; it was preserved: {alias}", file=sys.stderr)
        else:
            removed = True
    if removed:
        for directory in (project / ".opencode/skills", project / ".opencode"):
            if not directory.is_symlink():
                try:
                    directory.rmdir()
                except OSError:
                    pass  # Keep Agent configuration, reference files and user content.


def main():
    parser = argparse.ArgumentParser(description="Install the precision debugging Skill and CANN knowledge queries in the current project or globally.")
    parser.add_argument("client", choices=ADAPTERS)
    parser.add_argument("project", nargs="?", type=Path, help="existing project directory (default: current directory)")
    parser.add_argument("--global", dest="global_install", action="store_true", help="install for the current user across projects")
    parser.add_argument("--python", help="Python for installation and checks; defaults to CANNBOT_PYTHON or python3")
    parser.add_argument("--package-root", type=Path, help="reuse a local tool-precision-debug release")
    parser.add_argument("--knowledge-root", type=Path, help="reuse a complete local knowledge repository without automatic updates")
    parser.add_argument("--platform", help="optional default platform ID for project installations")
    parser.add_argument("--without-knowledge", action="store_true", help="install only the precision Skill")
    parser.add_argument("--with-sources", choices=("docs", "code", "all"), help="also fetch original documentation or operator sources")
    parser.add_argument("--offline", action="store_true", help="use existing local files and dependencies; local index building is allowed")
    parser.add_argument("--update", action="store_true", help="update the managed Skill and knowledge repository")
    parser.add_argument("--repository", default=PACKAGE_REPOSITORY, help="Git repository containing the precision release")
    parser.add_argument("--ref", default=PACKAGE_REF, help="branch containing the precision release")
    parser.add_argument("--skill-path", default=f"skills/{SKILL}/skills/{SKILL}", help="relative path of the pure Skill inside the repository")
    parser.add_argument("--knowledge-repository", default="https://gitcode.com/cann/cannbot-knowledge.git")
    parser.add_argument("--knowledge-ref", default="master")
    args = parser.parse_args(sys.argv[2:])
    skill_path = PurePosixPath(args.skill_path)
    if (not skill_path.parts or skill_path.is_absolute() or ".." in skill_path.parts
            or re.search(r"[\x00-\x1f*?\[\]\\]", args.skill_path)):
        parser.error("--skill-path must be a relative repository path without wildcards.")
    if sys.version_info < (3, 10):
        parser.error("Python 3.10 or newer is required.")
    if args.offline and (args.update or args.with_sources):
        parser.error("--offline cannot be combined with --update or --with-sources.")
    if args.without_knowledge and (args.with_sources or args.platform or args.knowledge_root):
        parser.error("--without-knowledge cannot be combined with knowledge or platform options.")
    if args.global_install and args.project is not None:
        parser.error("--global cannot be combined with a project directory; omit the directory argument.")
    if args.global_install and args.platform:
        parser.error("--platform is a project setting; select the target platform when querying a global installation.")
    project = (Path.home() if args.global_install else args.project or Path.cwd()).expanduser().resolve(strict=True)
    if not project.is_dir():
        parser.error("Installation root must be a directory.")
    skill_directory = global_skill_directory(args.client, project) if args.global_install else project / ".agents/skills"
    link_root = skill_directory.parent if args.global_install else project
    # The independent knowledge-query dependency still reads this fixed path.
    config = project / ".cannbot/knowledge.env"
    existing = {}
    if not args.without_knowledge:
        within_project(project, config)
        if config.is_symlink() or (config.exists() and not config.is_file()):
            raise ValueError("Knowledge configuration path is a symlink or directory; it was not overwritten.")
        existing = config_values(config)
    knowledge_checkout = (config.parent / "cannbot-knowledge").resolve()
    package = args.package_root
    if package is None and sys.argv[1] and Path(sys.argv[1]).is_file():
        adjacent = Path(sys.argv[1]).resolve().parent.parent / "skills" / SKILL
        if (adjacent / "SKILL.md").is_file():
            package = adjacent
    package = package.expanduser().resolve() if package else None
    remote_package = package is None
    legacy_packages = set()
    if remote_package:
        package = project / ".asc-tools" / SKILL
        legacy_packages = {(project / directory / PACKAGE_DIRECTORY / str(skill_path)).resolve()
                           for directory in (".cannbot/precision-debug", ".asc-tools/precision-debug")}
    knowledge = None
    managed_knowledge = False
    if not args.without_knowledge:
        if args.knowledge_root:
            knowledge = args.knowledge_root.expanduser().resolve()
        elif existing.get(ROOT_KEY):
            value = Path(existing[ROOT_KEY]).expanduser()
            knowledge = (value if value.is_absolute() else config.parent / value).resolve()
        elif os.environ.get(ROOT_KEY):
            knowledge = Path(os.environ[ROOT_KEY]).expanduser().resolve()
        else:
            knowledge = knowledge_checkout
        # Keep an existing managed knowledge checkout updateable at its old location.
        managed_knowledge = knowledge in {
            knowledge_checkout, (config.parent / "precision-debug/cannbot-knowledge").resolve(),
        }
        override = os.environ.get(ROOT_KEY)
        if override and Path(override).expanduser().resolve() != knowledge:
            raise ValueError("CANNBOT_KNOWLEDGE_ROOT conflicts with the project configuration; unset it in this shell and retry.")
    links = [(skill_directory / SKILL, package)]
    if knowledge:
        links += [(skill_directory / name, knowledge / ".agents/skills" / name) for name in QUERIES]
    canonical = list(links)
    if not args.global_install and ADAPTERS[args.client]:
        links += [(project / ADAPTERS[args.client] / target.name, target) for target, _ in canonical]
    precision_link = canonical[0][0]
    for target, source in links:
        preflight_link(link_root, target, source, legacy_packages if target == precision_link else ())
    if remote_package:
        within_project(project, package)
    if managed_knowledge:
        within_project(project, knowledge)
    if (remote_package or knowledge) and not shutil.which("git"):
        raise ValueError("Git is required to fetch repositories and prepare the query index.")
    if remote_package:
        install_package(project, package, args.repository, args.ref, skill_path, offline=args.offline, update=args.update)
    validate_package(package)
    if knowledge:
        if managed_knowledge:
            repository(knowledge, args.knowledge_repository, args.knowledge_ref, offline=args.offline, update=args.update)
        required = ["knowledge/index.md", "requirements.txt", "governance/contracts/knowledge.py", "governance/schemas/registries.yaml", ".agents/skills/knowledge-query/scripts/knowledge_query.py", ".agents/skills/knowledge-query/scripts/knowledge_index.py", ".agents/skills/ascendc-api-knowledge-query/scripts/resolve_resources.py"]
        required += [f".agents/skills/{name}/SKILL.md" for name in QUERIES]
        if any(not (knowledge / value).is_file() for value in required):
            raise ValueError("Incomplete knowledge repository; --knowledge-root must point to a complete cannbot-knowledge checkout.")
        check_runtime(knowledge, args.offline)
        if args.platform:
            import yaml
            registry = yaml.safe_load((knowledge / "governance/schemas/registries.yaml").read_text())
            if args.platform not in registry["platforms"]:
                raise ValueError("--platform is not registered in this knowledge repository.")
        if args.with_sources:
            print(f"Fetching source resources: {args.with_sources}", flush=True)
            run(["bash", str(knowledge / "get_resources.sh"), f"--{args.with_sources}"], env={**os.environ, "CANNBOT_PYTHON": sys.executable})
        index = knowledge / ".agents/skills/knowledge-query/scripts/knowledge_index.py"
        check = subprocess.run([sys.executable, str(index), "--knowledge-root", str(knowledge), "verify", "--level", "quick"], capture_output=True)
        if check.returncode:
            print("Building knowledge query index...", flush=True)
            run([sys.executable, str(index), "--knowledge-root", str(knowledge), "build"])
        run([sys.executable, str(index), "--knowledge-root", str(knowledge), "verify", "--level", "quick"])
        query = knowledge / ".agents/skills/knowledge-query/scripts/knowledge_query.py"
        run([sys.executable, str(query), "--knowledge-root", str(knowledge), "discover"], cwd=project)
    created = []
    try:
        if knowledge:
            within_project(project, config)
            if config.is_symlink() or (config.exists() and not config.is_file()):
                raise ValueError("Knowledge configuration path changed during preparation; project entries were not written.")
        for target, source in links:
            preflight_link(link_root, target, source, legacy_packages if target == precision_link else ())
            if target.is_dir() and target.resolve() == source.resolve():
                continue
            previous_link = os.readlink(target) if target.is_symlink() else None
            target.parent.mkdir(parents=True, exist_ok=True)
            try:
                with tempfile.TemporaryDirectory(prefix=".skill-link-", dir=target.parent) as temporary:
                    staged = Path(temporary) / target.name
                    staged.symlink_to(os.path.relpath(source, target.parent.resolve()), target_is_directory=True)
                    os.replace(staged, target)
            except OSError as exc:
                raise ValueError(f"Cannot create Skill link: {target} ({exc.strerror})") from exc
            created.append((target, source, previous_link))
        if knowledge:
            write_config(config, knowledge, args.platform)
    except BaseException:
        for target, source, previous_link in reversed(created):
            if target.is_symlink() and target.resolve() == source.resolve():
                target.unlink()
                if previous_link is not None:
                    target.symlink_to(previous_link, target_is_directory=True)
        raise
    if not args.global_install and args.client == "opencode":
        remove_opencode_aliases(project, canonical)
    print(f"Installed{' globally' if args.global_install else ''}: {project}")
    print(f"{SKILL}: {precision_link}")
    print(f"Python: {sys.executable}")
    if knowledge:
        print(f"Knowledge: {knowledge}")
    print("Restart your client to load the Skill.")


try:
    main()
except (OSError, ValueError, subprocess.SubprocessError) as exc:
    print(f"Installation failed: {exc}", file=sys.stderr)
    raise SystemExit(2)
PY
