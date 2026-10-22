# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""gen_interface 祖先判定与工作区洁净检查的单测。

覆盖检视要求的三类边界：git 不可用（非 git 目录）、dirty worktree、
非法/不可解析 commit——三者都必须走保守回退（False / 判定不成立）。
"""

import importlib.util
import subprocess
from pathlib import Path

import pytest

TEST_ROOT = Path(__file__).resolve().parent
SKILL_ROOT = TEST_ROOT.parent
SCRIPT_PATH = SKILL_ROOT / "scripts" / "gen_interface.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("gen_interface", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


gen_interface = _load_module()


def _git(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


@pytest.fixture()
def git_repo(tmp_path: Path) -> Path:
    """两提交的最小 git 仓：base -> head。"""
    repo = tmp_path / "repo"
    repo.mkdir()
    _git(repo, "init", "-q")
    _git(repo, "config", "user.email", "t@t")
    _git(repo, "config", "user.name", "t")
    (repo / "main.py").write_text("base\n", encoding="utf-8")
    _git(repo, "add", ".")
    _git(repo, "commit", "-q", "-m", "base")
    (repo / "main.py").write_text("head\n", encoding="utf-8")
    _git(repo, "add", ".")
    _git(repo, "commit", "-q", "-m", "head")
    return repo


def test_ancestor_commit_of_head_returns_true(git_repo: Path):
    base = _git(git_repo, "rev-parse", "--short", "HEAD~1")
    assert gen_interface.commit_is_ancestor(git_repo, base) is True


def test_descendant_commit_returns_false(git_repo: Path):
    head = _git(git_repo, "rev-parse", "--short", "HEAD")
    _git(git_repo, "checkout", "-q", "HEAD~1")
    assert gen_interface.commit_is_ancestor(git_repo, head) is False


def test_invalid_commit_returns_false(git_repo: Path):
    assert gen_interface.commit_is_ancestor(git_repo, "deadbee") is False


def test_non_git_directory_returns_false(tmp_path: Path):
    assert gen_interface.commit_is_ancestor(tmp_path, "1234567") is False


def test_clean_worktree_returns_true(git_repo: Path):
    assert gen_interface.worktree_file_clean(git_repo, "main.py") is True


def test_modified_file_returns_false(git_repo: Path):
    (git_repo / "main.py").write_text("dirty\n", encoding="utf-8")
    assert gen_interface.worktree_file_clean(git_repo, "main.py") is False


def test_untracked_file_returns_false(git_repo: Path):
    (git_repo / "extra.py").write_text("new\n", encoding="utf-8")
    assert gen_interface.worktree_file_clean(git_repo, "extra.py") is False


def test_non_git_directory_clean_returns_false(tmp_path: Path):
    (tmp_path / "main.py").write_text("x\n", encoding="utf-8")
    assert gen_interface.worktree_file_clean(tmp_path, "main.py") is False
