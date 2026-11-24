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
command -v "$python_bin" >/dev/null 2>&1 || { echo 'Python 3.10 or newer is required. Select it with --python.' >&2; exit 2; }
exec "$python_bin" - "${BASH_SOURCE[0]:-}" "$@" <<'PY'
import sys

if sys.version_info < (3, 10):
    sys.exit('Python 3.10 or newer is required.')

import argparse
import errno
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import subprocess
import tempfile
from urllib.parse import urlsplit

SKILL = "tool-comm-debug"
PACKAGE_REPOSITORY = "https://gitcode.com/cann/asc-tools.git"
PACKAGE_REF = "master"
PACKAGE_DIRECTORY = "asc-tools"
QUERIES = ("knowledge-query", "ascendc-api-knowledge-query")
ADAPTERS = {"codex": None, "claude": ".claude/skills", "opencode": None}
ROOT_KEY = "CANNBOT_KNOWLEDGE_ROOT"
PLATFORM_KEY = "CANNBOT_KNOWLEDGE_PLATFORM"
REF_KEY = "CANNBOT_KNOWLEDGE_REF"


def global_skill_directory(client, home):
    if client == "codex":
        directory = home / ".agents"
    elif client == "claude":
        directory = Path(os.environ.get("CLAUDE_CONFIG_DIR") or home / ".claude")
    else:
        xdg = Path(os.environ.get("XDG_CONFIG_HOME") or home / ".config")
        directory = Path(os.environ.get("OPENCODE_CONFIG_DIR") or xdg / "opencode")
    directory = directory.expanduser()
    if not directory.is_absolute():
        raise ValueError("Global client configuration directory must be an absolute path.")
    return directory.resolve() / "skills"


def run(command, *, cwd=None, env=None):
    environment = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1", **(env or {})}
    result = subprocess.run(command, cwd=cwd, env=environment, text=True, capture_output=True)
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        detail = re.sub(r"(https?://)[^/\s@]+@", r"\1<redacted>@", detail)
        raise ValueError(f"{Path(command[0]).name} failed: {detail[-2000:]}")
    return result.stdout.strip()


def within_project(project, target):
    if not target.parent.resolve().is_relative_to(project):
        raise ValueError(f"Installation path escapes the project: {target}")
    for parent in target.parents:
        if parent == project:
            break
        if parent.exists() and not parent.is_dir():
            raise ValueError(f"Installation directory is occupied by a file: {parent}")


def normalized_url(url):
    return url.rstrip("/").removesuffix(".git").rstrip("/")


def verify_checkout_revision(path, ref, *, url=None, update=False):
    if not shutil.which("git"):
        raise ValueError("Git is required to fetch or verify Git repositories.")
    is_commit = bool(re.fullmatch(r"[0-9a-fA-F]{40}", ref))
    if is_commit:
        ref = ref.lower()
    else:
        run(["git", "check-ref-format", "--branch", ref])
    git = ["git", "-C", str(path)]
    if not path.is_dir() or Path(run([*git, "rev-parse", "--show-toplevel"])).resolve() != path.resolve():
        raise ValueError(f"Expected an independent Git repository at {path}")
    if url is not None and normalized_url(run([*git, "remote", "get-url", "origin"])) != normalized_url(url):
        raise ValueError(f"Repository origin does not match the requested source: {path}")
    head = run([*git, "rev-parse", "HEAD"]).lower()
    dirty = bool(run([*git, "status", "--porcelain", "--untracked-files=no"]))
    if is_commit:
        if head != ref:
            raise ValueError(f"Expected commit {ref} at {path}")
        if dirty:
            raise ValueError(f"Repository has local changes: {path}")
        return
    branch = run([*git, "branch", "--show-current"])
    if branch == ref:
        if update:
            if dirty:
                raise ValueError(f"Repository has local changes: {path}")
            print(f"Updating {path.name}...", flush=True)
            run([*git, "fetch", "origin", ref])
            run([*git, "merge", "--ff-only", "FETCH_HEAD"])
        return
    if not branch:
        tag_check = subprocess.run(
            [*git, "rev-parse", "--verify", "-q", f"refs/tags/{ref}^{{commit}}"],
            text=True, capture_output=True,
        )
        if tag_check.returncode == 0 and tag_check.stdout.strip().lower() == head:
            if dirty:
                raise ValueError(f"Repository has local changes: {path}")
            if update:
                print(f"Updating {path.name}...", flush=True)
                run([*git, "fetch", "--depth", "1", "origin", f"+refs/tags/{ref}:refs/tags/{ref}"])
                run([*git, "checkout", "--detach", "FETCH_HEAD"])
            return
    raise ValueError(f"Expected branch or tag {ref} at {path}")


def repository(path, url, ref, *, offline, update, sparse=None):
    parsed = urlsplit(url)
    if parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise ValueError("Use configured Git authentication instead of credentials or query parameters in repository URLs.")
    is_commit = bool(re.fullmatch(r"[0-9a-fA-F]{40}", ref))
    if is_commit:
        ref = ref.lower()
    if path.is_symlink():
        raise ValueError(f"Managed repository must not be a symlink: {path}")
    if path.exists():
        verify_checkout_revision(path, ref, url=url, update=update)
        return
    if offline:
        raise ValueError(f"Offline installation requires a local repository: {path}")
    if not shutil.which("git"):
        raise ValueError("Git is required to fetch or verify Git repositories.")
    if not is_commit:
        run(["git", "check-ref-format", "--branch", ref])
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".comm-clone-", dir=path.parent) as temporary:
        staged = Path(temporary) / "checkout"
        if is_commit:
            staged.mkdir(parents=True, exist_ok=True)
            git = ["git", "-C", str(staged)]
            run([*git, "init", "-q"])
            run([*git, "remote", "add", "origin", url])
            fetch_cmd = [*git, "fetch", "--depth", "1"]
            if sparse:
                fetch_cmd.append("--filter=blob:none")
            run([*fetch_cmd, "origin", ref])
            if sparse:
                run([*git, "sparse-checkout", "set", "--no-cone", f"/{sparse}/"])
            run([*git, "checkout", "--detach", "FETCH_HEAD"])
            if run([*git, "rev-parse", "HEAD"]).lower() != ref.lower():
                raise ValueError(f"Fetched commit does not match {ref}")
        else:
            command = ["git", "clone", "--depth", "1", "--single-branch", "--branch", ref]
            if sparse:
                command += ["--filter=blob:none", "--no-checkout"]
            run([*command, "--", url, str(staged)])
            if sparse:
                git = ["git", "-C", str(staged)]
                run([*git, "sparse-checkout", "set", "--no-cone", f"/{sparse}/"])
                run([*git, "checkout", ref])
        if path.exists() or path.is_symlink():
            raise ValueError(f"Destination appeared during cloning: {path}")
        staged.rename(path)


def package_entries(directory, *, strip_cache=False):
    if directory.is_symlink():
        raise ValueError(f"Skill content must not contain symlinks: {directory}")
    for path in sorted(directory.iterdir()):
        if path.is_symlink():
            raise ValueError(f"Skill content must not contain symlinks: {path}")
        if path.is_dir() and path.name == "__pycache__":
            if strip_cache:
                shutil.rmtree(path)
            continue
        if path.is_file() and path.suffix == ".pyc":
            if strip_cache:
                path.unlink()
            continue
        yield path
        if path.is_dir() and path.name != ".git":
            yield from package_entries(path, strip_cache=strip_cache)


def validate_package(package):
    files = {"SKILL.md", "evals/evals.json"}
    for path in package_entries(package):
        relative = path.relative_to(package)
        if relative.parts[0] == "references" and (path.is_dir() or (path.is_file() and path.suffix == ".md")):
            continue
        if path.is_dir() and relative.as_posix() == "evals":
            continue
        if not path.is_file() or relative.as_posix() not in files:
            raise ValueError(f"Unexpected entry in pure Skill: {relative}")
    entry = package / "SKILL.md"
    if not entry.is_file() or not re.search(rf"(?m)^name:[ \t]*{SKILL}[ \t]*$", entry.read_text(encoding="utf-8")):
        raise ValueError(f"Expected SKILL.md with name: {SKILL} in {package}")
    if not (package / "references").is_dir() or not (package / "evals/evals.json").is_file():
        raise ValueError("The pure Skill requires references/ and evals/evals.json.")


def package_digest(package):
    digest = hashlib.sha256()
    for path in package_entries(package):
        if path.name == ".git":
            continue
        digest.update(path.relative_to(package).as_posix().encode() + b"\0")
        digest.update(b"F" + hashlib.sha256(path.read_bytes()).digest() if path.is_file() else b"D")
    return digest.hexdigest()


def install_package(project, package, url, ref, skill_path, *, offline, update):
    state = package.parent / f".{SKILL}.json"
    within_project(project, package)
    if package.is_symlink() or state.is_symlink():
        raise ValueError("Managed Skill and installation state must not be symlinks.")
    canonical_ref = ref.lower() if re.fullmatch(r"[0-9a-fA-F]{40}", ref) else ref
    requested = {"repository": normalized_url(url), "ref": canonical_ref, "skill_path": skill_path.as_posix()}
    previous = None
    if package.exists() or state.exists():
        if not package.is_dir() or not state.is_file():
            raise ValueError("Existing Skill has no matching installation state.")
        previous = state.read_bytes()
        recorded = json.loads(previous)
        if (not isinstance(recorded, dict) or not isinstance(recorded.get("repository"), str)
                or normalized_url(recorded["repository"]) != requested["repository"]
                or any(recorded.get(key) != requested[key] for key in ("ref", "skill_path"))):
            raise ValueError("Installed Skill source does not match this installation.")
        validate_package(package)
        if package_digest(package) != recorded.get("digest"):
            raise ValueError("Managed Skill contains local changes.")
        if not update:
            return
    elif offline:
        raise ValueError(f"Offline installation requires an existing Skill: {package}")
    package.parent.mkdir(parents=True, exist_ok=True)
    print(f"{'Updating' if previous else 'Fetching'} {SKILL}...", flush=True)
    with tempfile.TemporaryDirectory(prefix=f".{SKILL}-", dir=package.parent) as temporary:
        staging = Path(temporary)
        checkout = staging / PACKAGE_DIRECTORY
        repository(checkout, url, ref, offline=False, update=False, sparse=skill_path.as_posix())
        downloaded = checkout / skill_path
        if downloaded.resolve() != checkout.resolve() / skill_path:
            raise ValueError("Downloaded Skill path contains a symlink.")
        for _ in package_entries(downloaded, strip_cache=True):
            pass
        validate_package(downloaded)
        staged_state = staging / "state.json"
        staged_state.write_text(json.dumps({**requested, "digest": package_digest(downloaded)}) + "\n", encoding="utf-8")
        within_project(project, package)
        if package.is_symlink() or state.is_symlink():
            raise ValueError("Installation paths changed during download.")
        if previous is None:
            if package.exists() or state.exists():
                raise ValueError("Installation destination appeared during download.")
        elif state.read_bytes() != previous or package_digest(package) != recorded["digest"]:
            raise ValueError("Installed Skill changed during download.")
        backup = staging / "previous"
        if previous is not None:
            package.rename(backup)
        try:
            downloaded.rename(package)
            os.replace(staged_state, state)
        except BaseException:
            if not downloaded.exists() and package.exists():
                shutil.rmtree(package)
            if backup.exists():
                backup.rename(package)
            raise


def check_config(project, config):
    within_project(project, config)
    if config.is_symlink() or (config.exists() and not config.is_file()):
        raise ValueError(f"Expected a regular knowledge configuration file: {config}")


def config_values(config):
    values = {}
    if config.is_file():
        for line in config.read_text(encoding="utf-8").splitlines():
            tokens = shlex.split(line, comments=True)
            if tokens[:1] == ["export"]:
                tokens = tokens[1:]
            if len(tokens) == 1 and "=" in tokens[0]:
                key, value = tokens[0].split("=", 1)
                values.setdefault(key, value)
    return values


def configured_root(config, value, managed):
    path = Path(value).expanduser()
    if path.is_absolute():
        return path
    candidates = (config.parent.parent / path, config.parent / path)
    for candidate in candidates:
        if (candidate / "knowledge/index.md").is_file():
            return candidate
    for candidate in candidates:
        if candidate in managed or candidate.exists():
            return candidate
    return candidates[0]


def knowledge_location(args, config):
    managed = (config.parent / "cannbot-knowledge", config.parent / "precision-debug/cannbot-knowledge")
    existing = config_values(config).get(ROOT_KEY)
    override = os.environ.get(ROOT_KEY)
    if args.knowledge_root:
        path = args.knowledge_root.expanduser()
    elif existing:
        path = configured_root(config, existing, managed)
    elif override:
        path = Path(override).expanduser()
    else:
        path = next((candidate for candidate in managed if candidate.is_dir()), managed[0])
    knowledge = path.resolve()
    if override and Path(override).expanduser().resolve() != knowledge:
        raise ValueError(f"{ROOT_KEY} conflicts with the selected knowledge root. Unset it or select the same directory.")
    is_managed = args.knowledge_root is None and knowledge in {candidate.resolve() for candidate in managed}
    if is_managed:
        for candidate in managed:
            if candidate.resolve() == knowledge:
                within_project(config.parent.parent, candidate)
                if candidate.is_symlink():
                    raise ValueError(f"Managed repository must not be a symlink: {candidate}")
    return knowledge, is_managed


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
        raise ValueError("Install the knowledge repository requirements.txt in the selected Python environment before offline installation.")
    print("Installing knowledge query dependencies...", flush=True)
    run([sys.executable, "-m", "pip", "install", "-r", str(knowledge / "requirements.txt")])
    if not ready():
        raise ValueError("Knowledge query dependencies are unavailable in the selected Python environment.")


def prepare_knowledge(project, knowledge, args):
    skills = knowledge / ".agents/skills"
    scripts = skills / "knowledge-query/scripts"
    registry = knowledge / "governance/schemas/registries.yaml"
    required = [knowledge / "knowledge/index.md", knowledge / "requirements.txt", registry,
                knowledge / "governance/contracts/knowledge.py", scripts / "knowledge_index.py",
                scripts / "knowledge_query.py", skills / "ascendc-api-knowledge-query/scripts/resolve_resources.py"]
    required.extend(skills / name / "SKILL.md" for name in QUERIES)
    for path in required:
        if not path.is_file():
            raise ValueError(f"Incomplete knowledge repository. Missing file: {path}")
    check_runtime(knowledge, args.offline)
    if args.platform:
        import yaml
        if args.platform not in yaml.safe_load(registry.read_text(encoding="utf-8"))["platforms"]:
            raise ValueError(f"Unknown platform in {registry}: {args.platform}")
    if args.with_sources:
        run(["bash", str(knowledge / "get_resources.sh"), f"--{args.with_sources}"], cwd=knowledge,
            env={"CANNBOT_PYTHON": sys.executable})
    index = [sys.executable, "-B", str(scripts / "knowledge_index.py"), "--knowledge-root", str(knowledge)]
    verify = [*index, "verify", "--level", "quick"]
    if subprocess.run(verify, capture_output=True).returncode:
        print("Building knowledge query index...", flush=True)
        run([*index, "build"])
        run(verify)
    run([sys.executable, str(scripts / "knowledge_query.py"), "--knowledge-root", str(knowledge), "discover"], cwd=project)


def write_config(path, knowledge, platform, knowledge_ref=None):
    updates = {ROOT_KEY: str(knowledge.resolve())}
    if platform:
        updates[PLATFORM_KEY] = platform
    if knowledge_ref:
        updates[REF_KEY] = knowledge_ref
    previous = path.read_bytes() if path.exists() else b""
    kept = [line for line in previous.decode("utf-8").splitlines()
            if not any(re.match(rf"^\s*(?:export\s+)?{key}=", line) for key in updates)]
    content = "\n".join([*kept, *(f"{key}={shlex.quote(value)}" for key, value in updates.items())]) + "\n"
    if previous == content.encode():
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=".knowledge-env-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(content)
        if path.exists():
            os.chmod(temporary, path.stat().st_mode & 0o777)
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def preflight_link(root, target, source, replace_sources):
    within_project(root, target)
    if target.exists() or target.is_symlink():
        if target.resolve() == source.resolve() or (target.is_symlink() and target.resolve() in replace_sources):
            return
        raise ValueError(f"Existing path conflicts with this installation: {target}")


def replace_link(target, value):
    with tempfile.TemporaryDirectory(prefix=".skill-link-", dir=target.parent) as temporary:
        staged = Path(temporary) / target.name
        staged.symlink_to(value, target_is_directory=True)
        os.replace(staged, target)


def remove_opencode_aliases(project, canonical):
    directory = project / ".opencode/skills"
    if directory.parent.is_symlink() or directory.is_symlink():
        return
    removed = False
    for target, _ in canonical:
        alias = directory / target.name
        if directory.resolve() == target.parent.resolve() or not alias.is_symlink() or alias.resolve() != target.resolve():
            continue
        try:
            within_project(project, alias)
            alias.unlink()
            removed = True
        except (OSError, ValueError) as exc:
            print(f"Could not remove old client link {alias}: {exc}", file=sys.stderr)
    if removed:
        for parent in (directory, directory.parent):
            if parent.is_symlink():
                break
            try:
                parent.rmdir()
            except OSError as exc:
                if exc.errno not in (errno.ENOTEMPTY, errno.EEXIST):
                    print(f"Could not remove directory {parent}: {exc}", file=sys.stderr)
                break


def main():
    parser = argparse.ArgumentParser(description="Install the communication debugging Skill and CANN knowledge queries.")
    parser.add_argument("client", choices=ADAPTERS)
    parser.add_argument("project", nargs="?", type=Path, help="existing project directory (default: current directory)")
    parser.add_argument("--global", dest="global_install", action="store_true", help="install for the current user across projects")
    parser.add_argument("--python", help="Python executable (default: CANNBOT_PYTHON or python3)")
    parser.add_argument("--package-root", type=Path, help="reuse a local pure Skill directory")
    parser.add_argument("--knowledge-root", type=Path, help="reuse a complete local knowledge repository without updating it")
    parser.add_argument("--platform", help="default platform ID for project queries")
    parser.add_argument("--without-knowledge", action="store_true", help="install only the communication debugging Skill")
    parser.add_argument("--with-sources", choices=("docs", "code", "all"), help="also fetch documentation or operator sources")
    parser.add_argument("--offline", action="store_true", help="use local packages and dependencies, allowing local index builds")
    parser.add_argument("--update", action="store_true", help="update the managed Skill and knowledge repository")
    parser.add_argument("--repository", default=PACKAGE_REPOSITORY, help="Git repository containing the pure Skill")
    parser.add_argument("--ref", default=PACKAGE_REF, help="Skill repository branch")
    parser.add_argument("--skill-path", default=f"skills/{SKILL}/skills/{SKILL}", help="pure Skill path inside the repository")
    parser.add_argument("--knowledge-repository", default="https://gitcode.com/cann/cannbot-knowledge.git")
    parser.add_argument("--knowledge-ref", help="knowledge repository branch, tag, or 40-char commit SHA")
    args = parser.parse_args(sys.argv[2:])
    skill_path = PurePosixPath(args.skill_path)
    if (not skill_path.parts or skill_path.is_absolute() or ".." in skill_path.parts
            or re.search(r"[\x00-\x1f*?\[\]\\]", args.skill_path)):
        parser.error("--skill-path must be a relative repository path without wildcards.")
    if args.offline and (args.update or args.with_sources):
        parser.error("--offline cannot be combined with --update or --with-sources.")
    if args.without_knowledge and (args.knowledge_root or args.platform or args.with_sources):
        parser.error("--without-knowledge cannot be combined with --knowledge-root, --platform or --with-sources.")
    if args.global_install and (args.project is not None or args.platform):
        parser.error("--global cannot be combined with a project directory or --platform.")
    project = (Path.home() if args.global_install else args.project or Path.cwd()).expanduser().resolve(strict=True)
    if not project.is_dir():
        parser.error("Installation root must be a directory.")
    directory = global_skill_directory(args.client, project) if args.global_install else project / ".agents/skills"
    link_root = directory.parent if args.global_install else project
    package = args.package_root
    if package is None and sys.argv[1] and Path(sys.argv[1]).is_file():
        adjacent = Path(sys.argv[1]).resolve().parent.parent / "skills" / SKILL
        if (adjacent / "SKILL.md").is_file():
            package = adjacent
    managed_package = package is None
    legacy = set()
    if managed_package:
        package = project / ".asc-tools" / SKILL
        legacy = {(project / parent / PACKAGE_DIRECTORY / skill_path).resolve()
                  for parent in (".cannbot/comm-debug", ".asc-tools/comm-debug")}
    else:
        package = package.expanduser().resolve()
        validate_package(package)
    config = project / ".cannbot/knowledge.env"
    knowledge = None
    explicit_knowledge_ref = None
    knowledge_ref = "master"
    canonical = [(directory / SKILL, package)]
    if not args.without_knowledge:
        check_config(project, config)
        knowledge, managed_knowledge = knowledge_location(args, config)
        explicit_knowledge_ref = args.knowledge_ref or os.environ.get(REF_KEY)
        knowledge_ref = explicit_knowledge_ref or config_values(config).get(REF_KEY) or "master"
        canonical.extend((directory / name, knowledge / ".agents/skills" / name) for name in QUERIES)
    links = list(canonical)
    if not args.global_install and ADAPTERS[args.client]:
        links.extend((project / ADAPTERS[args.client] / target.name, target) for target, _ in canonical)
    skill_link = canonical[0][0]
    for target, source in links:
        preflight_link(link_root, target, source, legacy if target == skill_link else ())
    if managed_package:
        install_package(project, package, args.repository, args.ref, skill_path, offline=args.offline, update=args.update)
    if knowledge:
        if managed_knowledge:
            repository(knowledge, args.knowledge_repository, knowledge_ref, offline=args.offline, update=args.update)
        elif explicit_knowledge_ref or config_values(config).get(REF_KEY):
            verify_checkout_revision(knowledge, knowledge_ref)
        prepare_knowledge(project, knowledge, args)
    created = []
    try:
        for target, source in links:
            preflight_link(link_root, target, source, legacy if target == skill_link else ())
            if target.is_dir() and target.resolve() == source.resolve():
                continue
            previous = os.readlink(target) if target.is_symlink() else None
            target.parent.mkdir(parents=True, exist_ok=True)
            value = os.path.relpath(source, target.parent.resolve())
            replace_link(target, value)
            created.append((target, value, previous))
        if knowledge:
            check_config(project, config)
            write_config(config, knowledge, args.platform, knowledge_ref=explicit_knowledge_ref)
    except BaseException:
        for target, value, previous in reversed(created):
            if target.is_symlink() and os.readlink(target) == value:
                if previous is None:
                    target.unlink()
                else:
                    replace_link(target, previous)
        raise
    if not args.global_install and args.client == "opencode":
        remove_opencode_aliases(project, canonical)
    print(f"Installed{' globally' if args.global_install else ''}: {project}")
    print(f"{SKILL}: {skill_link}")
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
