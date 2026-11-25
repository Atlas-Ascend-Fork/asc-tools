#!/usr/bin/env python3
# -*- coding: UTF-8 -*-
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""Resolve outer distribution root and inner pure Skill root for package tests."""

from __future__ import annotations

import os
from pathlib import Path

SKILL_NAME = "tool-compile-debug"


def resolve_roots(test_file: str | Path) -> tuple[Path | None, Path]:
    """Return (dist_root, skill_root) for both generated two-layer packages and isolated checks."""
    env_skill = os.environ.get("ACD_TEST_SKILL_ROOT")
    env_dist = os.environ.get("ACD_TEST_DIST_ROOT")
    if env_skill:
        skill_root = Path(env_skill).resolve()
        dist_root = Path(env_dist).resolve() if env_dist else None
        return dist_root, skill_root

    tests_dir = Path(test_file).resolve().parent
    parent = tests_dir.parent
    nested = parent / "skills" / SKILL_NAME
    if (nested / "SKILL.md").is_file():
        return parent, nested
    if (parent / "SKILL.md").is_file():
        return None, parent
    return parent, nested
