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
"""log_gap.py — ops-family 知识缺口计数写入（REVIEW2 G9 / 反馈 P1-2 ③，提前骑行项）

设计对齐：
  - ops-family SKILL.md 承诺「未收录族返回并**计数上报**（知识缺口感知回路的输入，
    落盘 references/knowledge_gaps.json）」——此前该文件六条缺口 count 全 0，
    无任何脚本写入：计数是纸面承诺（反馈 §4：「log_gap.py 提前——0.5 人日，
    让 knowledge_gaps 计数从 M1 起开始积累，否则 P1-7 的季度排产承诺持续空转」）。
  - 本脚本就是写入载体：路由时发现族未收录 / 族内文档为骨架 → 本条命令落一条
    gap 记录（count+1 / first_seen 登记 / note 追加），季度排产（27-4）读它。

用法：
  python3 log_gap.py --family fa --kind family_missing [--note "本次场景：FA 混合绑核"]
  python3 log_gap.py --list

gap 条目语义（与 knowledge_gaps.json 现有 schema 一致）：
  family    算子族名（fa/elementwise/sort/simt/matmul/mc2/...）
  kind      family_missing | guide_missing | guide_skeleton | no_shelf_code
            （未知 kind 允许写入——新类型由使用者定义，list 输出如实呈现）
  count     命中次数（排产权重）
  first_seen 首次命中日期
  last_seen 最近命中日期（本脚本新增字段；旧条目首次追加时补写）
  notes[]   每次命中的场景备注（增量追加，不覆盖）
"""

from __future__ import annotations

import argparse
import datetime
import json
import sys
from pathlib import Path

# knowledge_gaps.json 归 ops-family（族表 SKILL.md 承诺的落盘位置）；脚本放
# ops-mechanism/scripts 是因为「路由时发现未收录」发生在机制路由阶段（Step 3
# 查模板路径），ops-mechanism 是调用方。parents[2] = 套件根（tool-ops-tune/）。
GAPS_PATH = (
    Path(__file__).resolve().parents[2]
    / "ops-family"
    / "references"
    / "knowledge_gaps.json"
)

KNOWN_KINDS = ("family_missing", "guide_missing", "guide_skeleton", "no_shelf_code")


def load_gaps() -> dict:
    if not GAPS_PATH.exists():
        return {
            "schema": "opstune.knowledge-gaps/1.0",
            "description": "ops-family 知识缺口计数落盘",
            "gaps": [],
        }
    return json.loads(GAPS_PATH.read_text(encoding="utf-8"))


def save_gaps(gaps: dict) -> None:
    GAPS_PATH.parent.mkdir(parents=True, exist_ok=True)
    GAPS_PATH.write_text(
        json.dumps(gaps, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def main() -> int:
    ap = argparse.ArgumentParser(
        description="知识缺口计数写入（知识缺口感知回路 27-4 的输入）"
    )
    ap.add_argument("--family", help="算子族名（如 fa / elementwise）")
    ap.add_argument(
        "--kind",
        help="缺口类型（family_missing/guide_missing/guide_skeleton/no_shelf_code，"
        "或自定义新类型）",
    )
    ap.add_argument("--note", default=None, help="本次命中场景备注（追加进 notes）")
    ap.add_argument("--list", action="store_true", help="列出全部缺口与计数")
    args = ap.parse_args()

    gaps = load_gaps()

    if args.list or (not args.family and not args.kind):
        if not gaps.get("gaps"):
            print("（无缺口记录）")
            return 0
        print(f"{'family':<14} {'kind':<16} {'count':>5}  first_seen   last_seen")
        for g in sorted(gaps["gaps"], key=lambda g: -g.get("count", 0)):
            print(
                f"{g['family']:<14} {g['kind']:<16} {g.get('count', 0):>5}"
                f"  {g.get('first_seen', '-'):<11} {g.get('last_seen', '-'):<10}"
            )
        return 0

    if not args.family or not args.kind:
        print("ERROR: 需要 --family 与 --kind（或 --list）", file=sys.stderr)
        return 1
    if args.kind not in KNOWN_KINDS:
        # 不拒绝：新类型是正常演进，但 stderr 提示核对（防拼写错误把计数写进错误桶）
        print(
            f"WARN: kind {args.kind!r} 不在已知四类 {KNOWN_KINDS}——若是新类型忽略本提示；"
            f"若是笔误请改正后重跑（写错桶会稀释排产信号）",
            file=sys.stderr,
        )

    today = datetime.date.today().isoformat()
    entry = next(
        (
            g
            for g in gaps["gaps"]
            if g["family"] == args.family and g["kind"] == args.kind
        ),
        None,
    )
    if entry is None:
        entry = {
            "family": args.family,
            "kind": args.kind,
            "note": "",
            "first_seen": today,
            "count": 0,
            "notes": [],
        }
        gaps["gaps"].append(entry)
        print(f"[new] {args.family}/{args.kind} 条目登记")
    else:
        print(f"[hit] {args.family}/{args.kind}")
    entry["count"] = entry.get("count", 0) + 1
    entry["last_seen"] = today
    if args.note:
        entry.setdefault("notes", []).append(f"{today} {args.note}")
    save_gaps(gaps)
    print(f"count={entry['count']}  → {GAPS_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
