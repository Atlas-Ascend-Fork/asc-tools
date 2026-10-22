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
"""case_backfill.py — E 级案例回灌草稿生成（REVIEW2 G9 / 反馈 P1-2 ②）

设计对齐：
  - io-contracts.md 已冻结 case_candidate.json 契约（opstune.case-candidate/1.0，
    Step 5 产出，「经评审后入库」），但案例卡的入库一直是人工从 JSON 誊写
    Markdown——格式漂移、警示列漏写、证据等级漏标都是人工誊写的常态错误。
  - 本脚本把候选 JSON 转成 ops-mechanism/cases/ 案例卡 Markdown **草稿**：
    固定字段全带上 + E 级标注 + 警示列模板 + 「评审后入库」水印——仍是草稿，
    评审与合入是人工动作（与 calibrate.py 同款纪律：脚本不替代评审）。

用法：
  python3 case_backfill.py --input tuning_artifacts/gelu/case_candidate.json \
      --cases-dir ../cases -o cases/ascendc/case_gelu_ub_fusion.draft.md
  # 多条目输入（数组形态）也支持：逐条产出 draft

输出草稿字段（对齐 ops-mechanism SKILL.md 案例卡 schema）：
  算子类别 / DSL / 类型（PR|非PR） / 来源可信度（E 级） / 来源链接 + 验证日期 /
  瓶颈证据 / 优化机制 / 原文性能数字 / 适用范围与警示
"""

from __future__ import annotations

import argparse
import datetime
import json
import sys
from pathlib import Path


def as_candidates(data) -> list[dict]:
    """单条目 dict 或 entries 数组都接受。"""
    if isinstance(data, list):
        return data
    if isinstance(data, dict):
        if "entries" in data and isinstance(data["entries"], list):
            return data["entries"]
        return [data]
    return []


def scene_fp_text(fp: dict) -> str:
    if not isinstance(fp, dict):
        return str(fp)
    keys = ("soc", "shape", "dtype", "format", "cann_version", "timing_method")
    return " / ".join(f"{k}={fp[k]}" for k in keys if fp.get(k) is not None) or str(fp)


def draft_md(c: dict, idx: int) -> str:
    fp = c.get("scene_fingerprint", {})
    measured = c.get("speedup_measured")
    est = c.get("speedup_est")
    est_text = (
        f"[{est[0]}, {est[1]}]"
        if isinstance(est, (list, tuple)) and len(est) == 2
        else str(est)
    )
    rollback = c.get("rollback_happened")
    lines = [
        f"# 案例卡（草稿 {idx}）：{c.get('operator', '?')} — {c.get('mechanism', '?')}",
        "",
        "> ⚠️ **E 级回灌草稿——评审后入库**：本文件由 case_backfill.py 从 case_candidate.json",
        "> 自动生成，字段按 ops-mechanism SKILL.md 案例卡 schema 组装；警示列与适用边界",
        "> 为模板占位，评审时必须补全/核实后才可摘掉草稿水印。",
        "",
        "| 字段 | 值 |",
        "|------|-----|",
        f"| 算子类别 | {c.get('operator', '?')}（{c.get('dsl', 'Ascend C')}） |",
        "| 类型 | 非 PR（Agent 实测回流） |",
        "| 来源可信度 | **E 级**（本工作流 verdict→机制→实测闭环；speedup 只绑定当时口径指纹，不外推） |",
        f"| 来源链接 + 验证日期 | case_candidate.json @ {c.get('generated_at', '?')}；"
        f"生成于 {datetime.date.today().isoformat()} |",
        f"| 瓶颈证据 | rule_id `{c.get('bound_rule', '?')}`（verdict 命中，"
        f"口径指纹：{scene_fp_text(fp)}） |",
        f"| 优化机制 | {c.get('mechanism', '?')} |",
        f"| 原文性能数字 | speedup **{measured if measured is not None else '—'}**（实测，"
        f"KEEP 轮；预估区间 {est_text}） |",
        f"| 回滚 | {'**发生过**（见 notes）' if rollback else '无'} |",
        "",
        "## 适用范围与警示（评审时补全，占位模板）",
        "",
        "- [ ] 口径边界：上述 speedup 仅在「{soc} / {shape} / {dtype} / CANN {cann_version}」"
        "下成立，**禁止外推**到其他 shape/dtype/soc；".format(
            soc=fp.get("soc", "?"),
            shape=fp.get("shape", "?"),
            dtype=fp.get("dtype", "?"),
            cann_version=fp.get("cann_version", "?"),
        ),
        "- [ ] 触发条件：命中 {rule} 时的哪个反证/severity 档位下适用本机制；".format(
            rule=c.get("bound_rule", "?")
        ),
        "- [ ] 反例边界：何种场景该机制会劣化（回滚轮次的 notes 是线索）；",
        f"- [ ] 原始备注：{c.get('notes', '（无）')}",
        "",
        "## 入库前检查单（ops-mechanism SKILL.md 检索规则）",
        "",
        "- [ ] 警示列已补全（反例与收益案例同等收录，警示列必填）",
        "- [ ] 验证日期已核对",
        "- [ ] 与既有同型案例（≤3）合并或分立已决策",
        "- [ ] 摘除本草稿头部「草稿」水印",
    ]
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(
        description="case_candidate.json → E 级案例卡草稿（回灌 27-1 的半自动段）"
    )
    ap.add_argument(
        "--input", required=True, help="case_candidate.json（单条目或 entries 数组）"
    )
    ap.add_argument(
        "--cases-dir",
        default=None,
        help="ops-mechanism/cases/ 目录（提示用；草稿默认不入库，评审后人工移入）",
    )
    ap.add_argument(
        "-o",
        "--output",
        required=True,
        help="草稿输出路径（多条目时为前缀，逐条 .draft-N.md）",
    )
    args = ap.parse_args()

    data = json.loads(Path(args.input).read_text(encoding="utf-8"))
    cands = as_candidates(data)
    if not cands:
        print("ERROR: 输入无有效条目", file=sys.stderr)
        return 1
    schema = data.get("schema") if isinstance(data, dict) else None
    if schema and schema != "opstune.case-candidate/1.0":
        print(
            f"WARN: schema {schema} != opstune.case-candidate/1.0，仍按字段尽力解析",
            file=sys.stderr,
        )

    out = Path(args.output)
    written = []
    if len(cands) == 1:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(draft_md(cands[0], 1), encoding="utf-8")
        written.append(out)
    else:
        for i, c in enumerate(cands, 1):
            p = out.parent / f"{out.stem}.draft-{i}.md"
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(draft_md(c, i), encoding="utf-8")
            written.append(p)

    for p in written:
        print(f"draft → {p}")
    print(
        "下一步：人工评审（检查单在草稿尾部）→ 补全警示列 → 移入 cases/ 并摘水印"
        + (f"（目标目录参考：{args.cases_dir}）" if args.cases_dir else "")
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
