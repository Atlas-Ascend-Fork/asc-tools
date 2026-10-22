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
"""diff_audit.py — 实施范围机器校验（REVIEW2 G10 / 反馈 P1-3 ①）

设计对齐：
  - 单变量铁律与 diff 范围此前完全靠 perf-implementer 自报（"diff 摘要（改动点
    列表）"），无机器校验——多机制混入导致的归因失败（KEEP 了无法归因的收益）
    无机制拦截。
  - 本脚本用目录 diff + 方案卡 diff_sketch 对照做机器校验：改动文件集超出
    方案卡声明的范围 → 该轮标 UNATTRIBUTABLE，禁止进 KEEP 仲裁（退出码 2）。
  - 与 perf-reviewer subagent 的分工：本脚本只做**范围**校验（哪些文件被动了），
    代码质量与模板保真（MicroAPI VF 是否被降级）由 perf-reviewer subagent
    独立审查（P1-3 ②）。

用法：
  python3 diff_audit.py --baseline <原工程目录> --opt <优化副本目录> \
      [--candidates candidates.json --index 0] [-o audit.json]

方案卡对照（--candidates 提供时）：
  candidates.json 的条目可带 diff_sketch_files（数组：预期改动的文件/目录，
  相对工程根，支持目录前缀匹配）——有声明则逐文件对照；未声明时只输出
  改动清单（audit 记录在案，不判 UNATTRIBUTABLE——不能拿不存在的声明当红线）。

判定：
  exit 0  范围内（或未声明范围：记录不判罚）
  exit 2  超范围（UNATTRIBUTABLE：该轮收益不可归因到方案卡机制，禁进 KEEP 仲裁）

排除项（测量设施与产物，非优化变量——ops-collect 计时口径表既有纪律）：
  *.o / *.so / *.d / build/ 输出 / .git / __pycache__ / result.json /
  comparison.md / collection_manifest.json / PROF_* / *.log
"""

from __future__ import annotations

import argparse
import datetime
import json
import subprocess
import sys
from pathlib import Path

SCHEMA = "opstune.diff-audit/1.0"

# 测量设施与构建产物：复制目录带入的 harness / 采集产物 / 编译中间物不属优化变量
# （ops-collect 计时口径表既有纪律：harness 属测量设施而非优化变量）
EXCLUDE_NAMES = {
    "result.json",
    "comparison.md",
    "collection_manifest.json",
    "verdict.json",
    "audit.json",
    "scalars.json",
    "timing_harness.log",
    "iso_result.json",
    "perf_data.csv",
    "scope.json",
    # 计时 harness 文件（timing_harness 货架模板拷入：测量设施非优化变量）
    "timing_harness_aclrt_event.cpp",
    "timing_harness_torch_event.py",
}
EXCLUDE_PARTS = {
    "build",
    ".git",
    "__pycache__",
    "out",
    "prof",
    "tuning_artifacts",
    "mindstudio_profiler_output",
    "PROF_DATA",
    "node_modules",
}


def is_excluded(rel: str) -> bool:
    p = Path(rel)
    if p.name in EXCLUDE_NAMES:
        return True
    if p.suffix in (".o", ".so", ".d", ".log", ".pyc"):
        return True
    return any(part in EXCLUDE_PARTS for part in p.parts)


def diff_files(baseline: Path, opt: Path) -> dict[str, str]:
    """git diff --no-index --name-status（M/A/D 三态；R 罕见按 M 处理）。"""
    proc = subprocess.run(
        [
            "git",
            "diff",
            "--no-index",
            "--name-status",
            "--no-renames",
            str(baseline),
            str(opt),
        ],
        capture_output=True,
        text=True,
    )
    out: dict[str, str] = {}
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line or "\t" not in line:
            continue
        status, path = line.split("\t", 1)
        # path 形如 <baseline>/xxx 或 <opt>/xxx —— 剥掉前缀取相对路径
        for root in (baseline, opt):
            prefix = str(root).rstrip("/") + "/"
            if path.startswith(prefix):
                rel = path[len(prefix) :]
                status = (
                    "D"
                    if status.startswith("D")
                    else ("A" if status.startswith("A") else "M")
                )
                out[rel] = status
                break
    return out


def declared_files(card: dict) -> list[str] | None:
    """方案卡声明的改动文件集（diff_sketch_files；旧卡无此字段 → None）。"""
    v = card.get("diff_sketch_files")
    if isinstance(v, list) and v and all(isinstance(x, str) for x in v):
        return v
    return None


def matches_declared(rel: str, declared: list[str]) -> bool:
    """rel 命中声明集：精确路径或目录前缀（声明 'op_kernel/' 覆盖其下全部）。"""
    p = Path(rel)
    for d in declared:
        d = d.strip().strip("/")
        if not d:
            continue
        if rel == d or str(p) == d:
            return True
        if d.endswith("/") and rel.startswith(d):
            return True
        if not d.endswith("/") and (rel.startswith(d + "/")):
            return True
    return False


def main() -> int:
    ap = argparse.ArgumentParser(
        description="实施范围机器校验（单变量铁律的 diff 侧闸门）"
    )
    ap.add_argument(
        "--baseline", required=True, help="原工程目录（perf-implementer 复制源）"
    )
    ap.add_argument("--opt", required=True, help="优化副本目录")
    ap.add_argument(
        "--candidates", default=None, help="candidates.json（对照第 index 张方案卡）"
    )
    ap.add_argument("--index", type=int, default=0, help="方案卡序号（默认 0）")
    ap.add_argument(
        "--max-changed-lines",
        type=int,
        default=2000,
        help="改动行数上限（防爆改；超出按超范围处置）",
    )
    ap.add_argument("-o", "--output", default=None, help="audit.json 输出")
    args = ap.parse_args()

    baseline, opt = Path(args.baseline), Path(args.opt)
    if not baseline.is_dir() or not opt.is_dir():
        print(
            f"ERROR: --baseline/--opt 必须是目录（{baseline} / {opt}）", file=sys.stderr
        )
        return 1

    card = {}
    if args.candidates:
        data = json.loads(Path(args.candidates).read_text(encoding="utf-8"))
        cards = (
            data
            if isinstance(data, list)
            else data.get("candidates", data.get("cards", []))
        )
        if not isinstance(cards, list) or args.index >= len(cards):
            print(
                f"ERROR: candidates.json 无序号 {args.index} 的方案卡", file=sys.stderr
            )
            return 1
        card = cards[args.index]

    changed = {
        rel: st for rel, st in diff_files(baseline, opt).items() if not is_excluded(rel)
    }
    # 统计改动行数（仅 M 文件；A/D 大文件同样计入 diff 语义但极少见）
    n_lines = 0
    if changed:
        proc = subprocess.run(
            [
                "git",
                "diff",
                "--no-index",
                "--numstat",
                "--no-renames",
                str(baseline),
                str(opt),
            ],
            capture_output=True,
            text=True,
        )
        for line in proc.stdout.splitlines():
            parts = line.split("\t")
            if len(parts) >= 3:
                try:
                    n_lines += int(parts[0]) + int(parts[1])
                except ValueError:
                    pass

    declared = declared_files(card)
    out_of_scope = []
    if declared is not None:
        out_of_scope = [rel for rel in changed if not matches_declared(rel, declared)]
    oversized = n_lines > args.max_changed_lines

    audit = {
        "schema": SCHEMA,
        "generated_at": datetime.datetime.now().isoformat(timespec="seconds"),
        "baseline": str(baseline),
        "opt": str(opt),
        "mechanism": card.get("mechanism"),
        "target_rule": card.get("target_rule"),
        "declared_files": declared,
        "changed_files": [
            {"file": rel, "status": st} for rel, st in sorted(changed.items())
        ],
        "changed_lines": n_lines,
        "out_of_scope": out_of_scope,
        "oversized": oversized,
        "verdict": "OK" if (not out_of_scope and not oversized) else "UNATTRIBUTABLE",
    }
    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(
            json.dumps(audit, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
        )
        print(f"audit → {out}")

    print(
        f"diff audit: {audit['verdict']}（{len(changed)} 文件 / {n_lines} 行"
        f"{'，方案卡未声明范围：仅记录' if declared is None else ''}）"
    )
    for rel, st in sorted(changed.items()):
        mark = "  ✗ 超范围" if rel in out_of_scope else ""
        print(f"  [{st}] {rel}{mark}")
    if out_of_scope:
        print(
            f"\nERROR: {len(out_of_scope)} 个文件超出方案卡 diff_sketch_files 声明范围——"
            "该轮收益不可归因（UNATTRIBUTABLE），禁止进 KEEP 仲裁；",
            file=sys.stderr,
        )
        print(
            "  处置：回滚超范围改动重测，或确认后把文件补进方案卡并说明为何属同一原子机制。",
            file=sys.stderr,
        )
        return 2
    if oversized:
        print(
            f"\nERROR: 改动 {n_lines} 行 > 上限 {args.max_changed_lines}——疑似混入无关改动"
            "（UNATTRIBUTABLE）。确属大改（如模板整体替换）时用 --max-changed-lines 重估。",
            file=sys.stderr,
        )
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
