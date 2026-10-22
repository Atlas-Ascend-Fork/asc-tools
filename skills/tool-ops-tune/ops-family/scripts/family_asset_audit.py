#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# ----------------------------------------------------------------------------------------------------------
"""family_asset_audit.py — ops-family 族表数据防漂移静态断言（F13 整改验收）

对 SKILL.md 族分类表中的「模板数」列做实测校验：模板数必须与
references/<族>/ 下 .h/.cpp/.template 文件的实际计数一致。
不一致 → exit 1（族表失真，模板路由第一跳数据错误）。

用法：python3 family_asset_audit.py   （在 ops-family SKILL 根目录或仓根执行）
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SKILL_MD = Path(__file__).resolve().parents[1] / "SKILL.md"
REFERENCES = Path(__file__).resolve().parents[1] / "references"

TEMPLATE_EXTS = (".h", ".cpp", ".template")


def actual_template_count(family: str) -> int:
    d = REFERENCES / family
    if not d.is_dir():
        return 0
    return sum(1 for p in d.rglob("*") if p.suffix in TEMPLATE_EXTS and p.is_file())


def main() -> int:
    text = SKILL_MD.read_text(encoding="utf-8")
    # 解析族表行：| <族> | ... | <模板数> | <状态> | <文档> |
    rows = re.findall(r"^\|\s*([a-z0-9_]+)\s*\|[^|]+\|[^|]+\|\s*(\d+)\s*\|", text, re.M)
    if not rows:
        print(
            "ERROR: SKILL.md 族表未解析出「模板数」列（表结构变化？）", file=sys.stderr
        )
        return 1
    failed = []
    for family, declared in rows:
        actual = actual_template_count(family)
        mark = "OK" if int(declared) == actual else "MISMATCH"
        print(f"[{mark}] {family}: 表={declared} 实测={actual}")
        if mark == "MISMATCH":
            failed.append(family)
    if failed:
        print(
            f"\n族表模板数与实测不一致：{', '.join(failed)} —— 模板路由第一跳数据失真，"
            f"请更新 SKILL.md 族表（检视 F13）",
            file=sys.stderr,
        )
        return 1
    print(f"\n{len(rows)} 族模板数全部与实测一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
