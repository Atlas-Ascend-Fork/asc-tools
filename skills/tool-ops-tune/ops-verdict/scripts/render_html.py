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
"""render_html.py — comparison_matrix.md → 静态 HTML 报告（REVIEW2 G16 / 反馈 P2-3）

设计对齐：
  - 20 case × 多轮的对比矩阵纯文本人读不动（REVIEW2 G16）；同类工具已有
    8 页交互式 HTML——本仓定位**静态 HTML**（数据已在 JSON/矩阵，不追交互，
    反馈 P2-3 措施原文：「数据已在 JSON，不追交互式」）。
  - 输入两种形态任选：
    ① batch_pipeline 的矩阵目录（逐 case 的 comparison_md + snapshot json——
       矩阵结论从 snapshot json 重读，保数值真源）
    ② 显式 --snapshots（若干 result.json + comparison.md 对）——手工形态。
  - 输出单文件 HTML（内联 CSS，零外部依赖，可离线打开 / 邮件附件 / 归档）。

包含区块：
  - 汇总卡片：geomean / KEEP / REVERT / 噪声带 / MISSING 计数
  - 逐 variant 热力表：case × speedup 着色（绿=KEEP、红=REVERT、灰=带内/缺）
  - 逐 case 明细折叠块（comparison.md 原文内嵌，<details> 展开查看）

用法：
  python3 render_html.py --matrix-dir tuning_artifacts/gelu/matrix -o report.html
"""

from __future__ import annotations

import argparse
import datetime
import html
import json
import math
import re
from pathlib import Path


def load_matrix(matrix_dir: Path) -> dict:
    """从矩阵目录重建逐 case 数据：{case: {variant: snapshot dict}} + 对比 md。"""
    cases: dict[str, dict] = {}
    comps: dict[tuple[str, str], str] = {}
    for case_dir in sorted(p for p in matrix_dir.iterdir() if p.is_dir()):
        cid = case_dir.name
        for snap in case_dir.glob("*.json"):
            data = json.loads(snap.read_text(encoding="utf-8"))
            variant = data.get("variant", snap.stem)
            cases.setdefault(cid, {})[variant] = data
        for comp in case_dir.glob("*_comparison.md"):
            variant = comp.name.replace("_comparison.md", "")
            comps[(cid, variant)] = comp.read_text(encoding="utf-8")
    return {"cases": cases, "comparisons": comps}


def conclusion_of(comp_md: str | None, speedup: float | None) -> str:
    if comp_md:
        if "提升" in comp_md and speedup and speedup > 1.05:
            return "KEEP"
        if "劣化" in comp_md:
            return "REVERT"
        if "噪声带内" in comp_md:
            return "NOISE"
        if "拒绝" in comp_md or "口径" in comp_md.split("## 结论")[0][:50]:
            pass
    if speedup is None:
        return "MISSING"
    return "NOISE"


def speedup_color(sp: float | None) -> str:
    if sp is None:
        return "#9e9e9e"  # 灰：无数据
    if sp >= 1.05:
        # 绿色渐变：越深收益越大（1.05 → 浅，≥1.5 → 深）
        t = min((sp - 1.05) / 0.45, 1.0)
        return f"rgba(46, 125, 50, {0.25 + 0.55 * t})"
    if sp <= 0.95:
        # 红色渐变：越深劣化越大
        t = min((0.95 - sp) / 0.25, 1.0)
        return f"rgba(198, 40, 40, {0.25 + 0.55 * t})"
    return "#e0e0e0"  # 带内：浅灰


def md_to_html(md_text: str) -> str:
    """comparison.md 的极简渲染：表原样 + 代码块 + 段落（不引 markdown 库，
    保持零依赖；已 html 转义后套结构）。"""
    out = []
    in_code = False
    for line in md_text.splitlines():
        e = html.escape(line)
        if line.strip().startswith("```"):
            out.append("</pre>" if in_code else "<pre>")
            in_code = not in_code
        elif in_code:
            out.append(e)
        elif line.startswith("|"):
            cells = [c.strip() for c in line.strip("|").split("|")]
            if set("".join(cells)) <= set("-: "):
                continue  # 分隔行
            out.append(
                "<tr>" + "".join(f"<td>{html.escape(c)}</td>" for c in cells) + "</tr>"
            )
        elif line.startswith("#"):
            lvl = len(line) - len(line.lstrip("#"))
            out.append(f"<h{min(lvl + 2, 6)}>{e.lstrip('# ')}</h{min(lvl + 2, 6)}>")
        else:
            out.append(f"<p>{e}</p>" if line.strip() else "")
    body = "\n".join(out)
    # 把首个 <tr> 前插表头结构（简化：全部 td）
    return re.sub(r"((?:<tr>.*?</tr>\n?)+)", r"<table>\1</table>", body)


def render(data: dict, title: str) -> str:
    cases = data["cases"]
    comps = data["comparisons"]
    variants = sorted(
        {v for c in cases.values() for v in c}, key=lambda s: (s != "baseline", s)
    )
    case_ids = sorted(cases)
    rows_speedup, statuses = {}, {}
    for cid in case_ids:
        base = cases[cid].get("baseline")
        for v, snap in cases[cid].items():
            if v == "baseline" or not base:
                continue
            b, a = base.get("kernel_avg_us"), snap.get("kernel_avg_us")
            sp = (b / a) if (b and a) else None
            rows_speedup[(cid, v)] = sp
            statuses[(cid, v)] = conclusion_of(comps.get((cid, v)), sp)

    # 汇总
    per_variant: dict[str, dict] = {}
    for v in variants:
        if v == "baseline":
            continue
        sps = [sp for (cid, vv), sp in rows_speedup.items() if vv == v and sp]
        sts = [st for (cid, vv), st in statuses.items() if vv == v]
        per_variant[v] = {
            "geomean": math.exp(sum(math.log(s) for s in sps) / len(sps))
            if sps
            else None,
            "keep": sts.count("KEEP"),
            "revert": sts.count("REVERT"),
            "noise": sts.count("NOISE"),
            "missing": sts.count("MISSING"),
            "n": len(sts),
        }

    css = """
    body { font-family: -apple-system, 'Segoe UI', 'PingFang SC', sans-serif; margin: 2rem auto;
           max-width: 1100px; color: #212121; background: #fafafa; }
    h1 { border-bottom: 2px solid #2e7d32; padding-bottom: .4rem; }
    .cards { display: flex; gap: 1rem; flex-wrap: wrap; margin: 1rem 0 2rem; }
    .card { background: #fff; border: 1px solid #e0e0e0; border-radius: 8px;
            padding: .8rem 1.2rem; min-width: 130px; box-shadow: 0 1px 2px rgba(0,0,0,.06); }
    .card .num { font-size: 1.6rem; font-weight: 600; }
    .card .lbl { color: #757575; font-size: .8rem; }
    table { border-collapse: collapse; background: #fff; margin: 1rem 0; width: 100%; }
    th, td { border: 1px solid #e0e0e0; padding: .35rem .6rem; font-size: .85rem; text-align: center; }
    th { background: #f5f5f5; }
    td.sp { font-weight: 600; }
    details { margin: .4rem 0; }
    summary { cursor: pointer; color: #1565c0; font-size: .9rem; }
    pre { background: #f5f5f5; padding: .6rem; border-radius: 4px; overflow-x: auto;
          font-size: .8rem; }
    .meta { color: #757575; font-size: .85rem; }
    """

    parts = [
        "<!DOCTYPE html><html><head><meta charset='utf-8'>",
        f"<title>{html.escape(title)}</title><style>{css}</style></head><body>",
        f"<h1>{html.escape(title)}</h1>",
        f"<p class='meta'>render_html.py 生成于 {datetime.datetime.now().isoformat(timespec='seconds')}"
        f"（静态报告，数据真源为矩阵目录 snapshot JSON）</p>",
    ]

    for v, s in per_variant.items():
        gm = f"{s['geomean']:.3f}x" if s["geomean"] else "—"
        parts.append(
            f"<div class='card'><div class='num'>{gm}</div><div class='lbl'>{html.escape(v)} geomean"
            f"（{s['n']} case）</div></div>"
            f"<div class='card'><div class='num' style='color:#2e7d32'>{s['keep']}</div>"
            f"<div class='lbl'>KEEP</div></div>"
            f"<div class='card'><div class='num' style='color:#c62828'>{s['revert']}</div>"
            f"<div class='lbl'>REVERT</div></div>"
            f"<div class='card'><div class='num' style='color:#9e9e9e'>{s['noise']}</div>"
            f"<div class='lbl'>噪声带内</div></div>"
            f"<div class='card'><div class='num'>{s['missing']}</div>"
            f"<div class='lbl'>MISSING</div></div>"
        )

    # 热力表
    parts.append(
        "<h2>逐 case 热力表</h2><table><tr><th>Case</th><th>Shape</th><th>dtype</th>"
        + "".join(f"<th>{html.escape(v)}</th>" for v in variants if v != "baseline")
        + "</tr>"
    )
    for cid in case_ids:
        base = cases[cid].get("baseline", {})
        parts.append(
            f"<tr><td>{html.escape(cid)}</td>"
            f"<td>{html.escape(str(base.get('shape', '')))}</td>"
            f"<td>{html.escape(str(base.get('dtype', '')))}</td>"
        )
        for v in variants:
            if v == "baseline":
                continue
            sp = rows_speedup.get((cid, v))
            st = statuses.get((cid, v), "MISSING")
            color = speedup_color(sp)
            txt = f"{sp:.3f}x" if sp else "—"
            parts.append(
                f"<td class='sp' style='background:{color}' title='{html.escape(st)}'>"
                f"{txt}</td>"
            )
        parts.append("</tr>")
    parts.append("</table>")
    parts.append(
        "<p class='meta'>着色：绿=KEEP（越深收益越大）；红=REVERT；"
        "浅灰=噪声带内；深灰=无数据。悬停看结论。</p>"
    )

    # 逐 case 明细折叠
    parts.append("<h2>逐 case 对比明细</h2>")
    for (cid, v), md in sorted(comps.items()):
        parts.append(
            f"<details><summary>{html.escape(cid)} × {html.escape(v)}</summary>"
            + md_to_html(md)
            + "</details>"
        )

    parts.append("</body></html>")
    return "\n".join(parts)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="对比矩阵 → 静态 HTML 报告（零依赖单文件）"
    )
    ap.add_argument("--matrix-dir", required=True, help="batch_pipeline 的矩阵输出目录")
    ap.add_argument("--title", default=None, help="报告标题（缺省取矩阵目录名）")
    ap.add_argument("-o", "--output", required=True, help="HTML 输出路径")
    args = ap.parse_args()

    matrix_dir = Path(args.matrix_dir)
    if not matrix_dir.is_dir():
        print(f"ERROR: {matrix_dir} 不是目录", file=sys.stderr)
        return 1
    data = load_matrix(matrix_dir)
    if not data["cases"]:
        print("ERROR: 矩阵目录无 case 子目录（先跑 batch_pipeline）", file=sys.stderr)
        return 1

    title = (
        args.title
        or f"OpsTune 调优对比报告 — {matrix_dir.parent.name}/{matrix_dir.name}"
    )
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(render(data, title), encoding="utf-8")
    n = sum(len(v) - (1 if "baseline" in v else 0) for v in data["cases"].values())
    print(f"html → {out}（{len(data['cases'])} case × {n} 对比，静态单文件零依赖）")
    return 0


if __name__ == "__main__":
    import sys

    sys.exit(main())
