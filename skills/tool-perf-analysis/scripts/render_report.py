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
# -*- coding: utf-8 -*-
"""render_report.py — diagnose bundle → 性能解读报告（Markdown + 单文件 HTML）

输入：diagnose.py 产出的 diagnose_bundle.json（facts + verdict + 机制卡正文）。
输出：report.md + report.html（同数据源双格式；HTML 零外部依赖、离线可开）。

报告五节结构（固定框架）：
  ① 基本信息（芯片 / 算子 / 采集口径）
  ② 瓶颈判定（verdict headline + findings 证据链 + 反证）
  ③ 证据（关键指标：逐 block 统计 / pipe 占用图 / stall 分布 / trace 汇总）
  ④ 优化候选方案（机制卡路由：每命中一卡，含收益区间/风险/验证/回滚）
  ⑤ 数据缺口与补采建议

可视化规范（dataviz skill）：sequential 单蓝色阶（pipe 占用按量级）、
status 色（good/moderate/critical）、hairline 网格、thin marks、
直接标注 + 表格视图双通道（数值不经 hover 也可读）。
"""

from __future__ import annotations

import argparse
import datetime
import html
import json
import re
from pathlib import Path

# ---- 调色板（dataviz 参考实例；已过 validate_palette.js 双模式检查）----
# 顺序色 categorical（pipe 身份，固定顺序不循环）：blue/orange/aqua/yellow/magenta/green/violet/red
CATEGORICAL = [
    "#2a78d6",
    "#eb6834",
    "#1baf7a",
    "#eda100",
    "#e87ba4",
    "#008300",
    "#4a3aa7",
    "#e34948",
]
CATEGORICAL_DARK = [
    "#3987e5",
    "#d95926",
    "#199e70",
    "#c98500",
    "#d55181",
    "#008300",
    "#9085e9",
    "#e66767",
]
# 状态色（固定语义，不承载序列身份）
STATUS = {
    "good": "#0ca30c",
    "light": "#0ca30c",
    "moderate": "#fab219",
    "critical": "#d03b3b",
    "watch": "#898781",
    "info": "#2a78d6",
}
STATUS_LABEL = {
    "critical": "严重",
    "moderate": "中等",
    "light": "轻度",
    "watch": "观察",
}

SEV_ORDER = {"critical": 0, "moderate": 1, "light": 2}
VERDICT_ZH = {
    "NEEDS_OPTIMIZATION": "存在可优化瓶颈",
    "NEAR_LIMIT": "接近性能上限",
    "NO_BOUND_FOUND": "未命中瓶颈规则",
    "INSUFFICIENT_DATA": "证据不足",
}

PIPE_ZH = {
    "cube": "Cube 矩阵",
    "vec": "Vector 向量",
    "scalar": "Scalar 标量",
    "mte1": "MTE1 搬入(L0→UB)",
    "mte2": "MTE2 搬入(GM→UB)",
    "mte3": "MTE3 搬出",
    "fixpipe": "Fixpipe 回写",
}

STALL_ZH = {
    "scoreboard_not_ready": "访存依赖等待",
    "nop_cycles": "空转周期",
    "register_bank_conflict": "寄存器 Bank 冲突",
    "resource_conflict": "资源冲突",
    "warp_level_sync": "warp 级同步等待",
    "divergence_stack_spill": "分支发散栈溢出",
    "ibuf_empty": "取指饥饿",
    "others": "其他",
}


def esc(s):
    return html.escape(str(s if s is not None else ""))


def fmt(v, nd=2):
    if v is None:
        return "—"
    if isinstance(v, float):
        if v == int(v) and abs(v) < 1e6:
            return str(int(v))
        return f"{v:.{nd}f}"
    return str(v)


# ============================================================ Markdown 渲染


def render_md(b: dict) -> str:
    facts, verdict = b["facts"], b["verdict"]
    op = facts.get("op") or {}
    hw = facts.get("hardware") or {}
    stats = facts.get("block_stats") or {}
    L = []
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")

    # ---- ① 基本信息 ----
    L.append("# NPU 算子性能诊断报告")
    L.append("")
    L.append(
        f"> 生成时间 {now} · tool-perf-analysis · 判定引擎 tool-ops-tune ops-verdict "
        f"（判定只出自规则引擎，本报告只解释不判定）"
    )
    L.append("")
    dev = hw.get("Device Info", {})
    aicore = hw.get("AI Core Information", {})
    meminfo = hw.get("Memory Information", {})
    chip = facts.get("chip") or (dev.get("chip info") or "—")
    arch = facts.get("arch") or (dev.get("arch info") or "")
    soc = facts.get("soc") or "—"
    cur, rated = op.get("Current Freq"), op.get("Rated Freq")
    freq_note = ""
    try:
        if (
            cur is not None
            and rated is not None
            and float(rated) > 0
            and float(cur) < float(rated) * 0.9
        ):
            freq_note = " ⚠️ 降频采集（数据失真，建议定频重采）"
    except (TypeError, ValueError):
        pass
    op_name = op.get("Op Name", "—")
    L.append("## 1. 基本信息")
    L.append("")
    L.append("| 项 | 值 |")
    L.append("|---|---|")
    L.append(f"| 芯片 | {chip}{' / ' + arch if arch else ''}（soc={soc}）|")
    if aicore:
        L.append(
            f"| 核数 | AIC {aicore.get('ai cube count', '—')} / AIV {aicore.get('ai vector count', '—')}，主频 {aicore.get('ai cube frequency(MHZ)', '—')} MHz |"
        )
    L.append(f"| 算子 | `{op_name}` |")
    L.append(f"| 类型 | {op.get('Op Type', '—')} |")
    td = op.get("Task Duration(us)")
    td_cell = (
        f"{fmt(td)} µs" if td is not None else "—（无有效 Task Log；逐核耗时见 §3.1）"
    )
    L.append(f"| Task Duration | {td_cell} |")
    L.append(f"| Block Dim | {fmt(op.get('Block Dim'))}（启动逻辑核数）|")
    if cur is not None:
        L.append(f"| 频率 | 当前 {fmt(cur)} / 额定 {fmt(rated)} MHz{freq_note} |")
    if meminfo:
        L.append(
            f"| HBM | {fmt(meminfo.get('hbm total(MB)'))} MB 总量 / 已用 {fmt(meminfo.get('hbm used(MB)'))} MB |"
        )
    src_tool = (
        "npu-compute (.npu-rep)"
        if b.get("source_tool") == "npu-compute"
        else "msopprof (PROF_*)"
    )
    L.append(f"| 采集工具 | {src_tool} |")
    L.append(f"| 采集路径 | `{b.get('source_path', '—')}` |")
    if op.get("aic_flops_theoretical"):
        L.append(
            f"| 理论算力 | AIC {fmt(op.get('aic_flops_theoretical'))} / AIV {fmt(op.get('aiv_flops_theoretical'))} TFLOPS(FP16) |"
        )
    L.append(
        f"| 判定结论 | **{VERDICT_ZH.get(verdict.get('verdict'), verdict.get('verdict'))}** — {verdict.get('headline', '')} |"
    )
    L.append("")
    L.append(
        "> 单次采集（single-shot）：结论为非稳态，稳态结论需 warmup≥10 / repeat≥5 / median 复采。"
    )
    L.append("")

    # ---- ② 瓶颈判定 ----
    L.append("## 2. 瓶颈判定（问题）")
    L.append("")
    findings = [f for f in verdict.get("findings", []) if f.get("status") == "HIT"]
    findings.sort(key=lambda f: (SEV_ORDER.get(f.get("severity"), 3),))
    if not findings:
        L.append(
            f"**{VERDICT_ZH.get(verdict.get('verdict'), verdict.get('verdict'))}**：{verdict.get('headline', '')}"
        )
        L.append("")
    else:
        L.append(
            f"共命中 **{len(findings)}** 项瓶颈规则（判定书：`verdict.json`，"
            f"规则包 L3→L2→L1→L0 分层瀑布，阈值溯源见各条 threshold_source）："
        )
        L.append("")
        for i, f in enumerate(findings, 1):
            sev = f.get("severity", "light")
            L.append(
                f"### 2.{i} {f.get('name_display') or f.get('name')}（{STATUS_LABEL.get(sev, sev)} · 置信度 {f.get('confidence', '—')}）"
            )
            L.append("")
            L.append(f"- **规则**：`{f.get('rule_id')}`（{f.get('layer', '')} 层）")
            L.append(
                f"- **证据**：{f.get('evidence_display') or f.get('evidence', '—')}"
            )
            if f.get("severity_note"):
                L.append(f"- **分级注记**：{f['severity_note']}")
            if f.get("speedup_range"):
                src = f.get("speedup_source", "")
                L.append(
                    f"- **预估收益区间**：×{f['speedup_range'][0]}~{f['speedup_range'][1]}"
                    f"（典型收益，{src}；非本算子预测）"
                )
            ces = f.get("counter_evidence") or []
            if ces:
                L.append("- **反证（未推翻判定，供复核）**：")
                for ce in ces:
                    note = ce.get("note", "")
                    if ce.get("unevaluated") and "反证指标缺失" not in note:
                        note += "（反证指标缺失，未评估）"
                    L.append(f"  - ⚠️ {note}")
            L.append("")
    # watch 档
    watches = [
        (s.get("rule_id"), s.get("watch"))
        for s in verdict.get("skipped", [])
        if s.get("status") == "NOT_HIT" and s.get("watch")
    ]
    if watches:
        L.append("**观察档（未达判定线，低成本先行项）**：")
        for rid, note in watches:
            L.append(f"- 👁 `{rid}`：{note}")
        L.append("")

    # ---- ③ 证据 ----
    L.append("## 3. 证据（关键指标）")
    L.append("")
    if stats:
        L.append("### 3.1 逐 block 流水占用（PipeUtilization）")
        L.append("")
        L.append(
            f"- 活跃 block 数：**{stats.get('blocks', '—')}**"
            f"（Block Dim {fmt(op.get('Block Dim'))} / 物理核 {fmt((facts.get('scalars') or {}).get('physical_cores'))}）"
        )
        if "time_us" in stats:
            t = stats["time_us"]
            L.append(
                f"- 逐核耗时：mean **{t['mean']}** / median {t['median']} / min {t['min']} / max **{t['max']}** µs；"
                f"长尾比 max/mean = **{t.get('tail_ratio')}**，离散度 (max−min)/max = **{t.get('imbalance_pct')}%**"
            )
        pipes = [(k[:-10], v) for k, v in stats.items() if k.endswith("_ratio_pct")]
        if pipes:
            pipes.sort(key=lambda kv: -kv[1]["mean"])
            L.append("")
            L.append("| 流水 | 均值 | min | max | 说明 |")
            L.append("|---|---|---|---|---|")
            for name, v in pipes:
                note = "⬅ 主导" if name == stats.get("max_pipe") else ""
                if v.get("side"):
                    note += f"（{v['side']} 侧）" if note else f"（{v['side']} 侧）"
                L.append(
                    f"| {PIPE_ZH.get(name, name)} | **{v['mean']}%** | {v['min']}% | {v['max']}% | {note} |"
                )
            ps = stats.get("pipe_sum_mean_pct")
            if ps is not None:
                is_msopprof = b.get("source_tool") == "msopprof"
                if 85 <= ps <= 115:
                    interp = "串行无重叠指纹（Double Buffer 首选）"
                elif ps > 115:
                    interp = (
                        "多 pipe 并行重叠（msopprof SIMT 口径常见）"
                        if is_msopprof
                        else "多 pipe 并行重叠（标量与搬运/计算并行执行）"
                    )
                else:
                    interp = "pipe 间有空泡"
                L.append("")
                L.append(f"- pipe 占比求和 = **{ps}%** → {interp}")
        if stats.get("mte2_active_bw_gbps"):
            bw = stats["mte2_active_bw_gbps"]
            scal = facts.get("scalars") or {}
            peak = scal.get("mte2_active_bw_pct_of_peak")
            gm = scal.get("gm_bw_usage_rate")
            note = f"，占峰值 {fmt(peak)}%" if peak is not None else ""
            note += f"（GM 读写总利用率 {fmt(gm)}%，Σ块口径）" if gm is not None else ""
            L.append(
                f"- MTE2 活跃带宽均值：**{bw['mean']} GB/s**{note} — 活跃期口径，全程平均 ≈ active_bw × ratio"
            )
        L.append("")
    pcs = facts.get("pcsampling")
    if pcs:
        L.append("### 3.2 PC 采样 stall 分布（PcSampling）")
        L.append("")
        L.append(
            f"采样总数 {pcs['samples_total']}（{pcs['address_rows']} 个 PC 行）；"
            f"**top stall：{STALL_ZH.get(pcs['top_stall'], pcs['top_stall'])} = {pcs['top_stall_pct']}%**"
        )
        L.append("")
        L.append("| stall 项 | 占比 |")
        L.append("|---|---|")
        for k, v in pcs["distribution_pct"].items():
            if v >= 0.05:
                L.append(f"| {STALL_ZH.get(k, k)} | {v}% |")
        L.append("")
    tr = facts.get("trace_summary")
    if tr and tr.get("sampled_lanes"):
        L.append("### 3.3 流水时间线采样（PipeTrace）")
        L.append("")
        L.append(
            f"采样 {tr['sampled_lanes']} 条核轨道（最多 6 个 Group，不代表全部核）："
        )
        L.append("")
        L.append("| 流水 | 忙碌总量（µs，采样核） |")
        L.append("|---|---|")
        for k, v in tr.get("pipe_busy_total_us", {}).items():
            L.append(f"| {k} | {v} |")
        L.append("")
    scal = facts.get("scalars") or {}
    scalar_rows = [
        ("Task Duration", scal.get("task_duration_us"), "µs"),
        ("Block Dim", scal.get("block_dim"), ""),
        ("物理核数（soc-spec）", scal.get("physical_cores"), ""),
        ("ICache 缺失率", scal.get("icache_miss_rate"), "%"),
        ("L2 命中率", scal.get("l2_hit_rate"), "%"),
        ("MTE2 活跃带宽占峰值", scal.get("mte2_active_bw_pct_of_peak"), "%"),
        ("GM 带宽利用率", scal.get("gm_bw_usage_rate"), "%"),
        ("top stall 占比", scal.get("top_stall_pct"), "%"),
    ]
    have = [r for r in scalar_rows if r[1] is not None]
    if have:
        L.append("### 3.4 判定标量（verdict --scalar 输入，含来源）")
        L.append("")
        L.append("| 标量 | 值 |")
        L.append("|---|---|")
        for name, v, unit in have:
            L.append(f"| {name} | **{fmt(v)}{unit}** |")
        L.append("")

    # ---- ④ 优化候选方案 ----
    L.append("## 4. 优化候选方案（机制路由）")
    L.append("")
    cards = b.get("mechanism_cards") or []
    if cards:
        L.append(
            "> 按 verdict 判定书 `mechanism_refs` 路由到 tool-ops-tune ops-mechanism 机制卡"
            "（八要素：原理/前提/收益/限制/精度风险/验证/回滚/模板）。"
            "单变量铁律：一次只实施一个机制。"
        )
        L.append("")
        # 同一机制卡被多条命中引用时只展开一次（条目内注明全部对应判定）
        seen_cards: dict[str, list[str]] = {}
        for c in cards:
            for cp in c.get("card_paths", []):
                seen_cards.setdefault(cp, []).append(c.get("rule_id", "?"))
        for i, c in enumerate(cards, 1):
            L.append(
                f"### 4.{i} {c.get('name')} → {', '.join(Path(p).stem for p in c.get('card_paths', []))}"
            )
            L.append("")
            L.append(
                f"- **对应判定**：`{c.get('rule_id')}`（{STATUS_LABEL.get(c.get('severity', ''), c.get('severity'))}，置信度 {c.get('confidence')}）"
            )
            if c.get("speedup_range"):
                L.append(
                    f"- **典型收益区间**：×{c['speedup_range'][0]}~{c['speedup_range'][1]}（非本算子预测）"
                )
            for path_s, content in zip(
                c.get("card_paths", []), c.get("card_contents", [])
            ):
                # 机制卡正文逐张内嵌（八要素全量）；同卡已被前条展开则跳过
                rules_sharing = seen_cards.get(path_s, [])
                if (
                    len(rules_sharing) > 1
                    and rules_sharing.index(c.get("rule_id", "?")) > 0
                ):
                    L.append(
                        f"- （`{Path(path_s).name}` 已在条目 {rules_sharing[0]} 展开）"
                    )
                    continue
                L.append("")
                L.append("<details>")
                L.append("")
                share_note = (
                    f"（同对应 {', '.join(r for r in rules_sharing[1:])}）"
                    if len(rules_sharing) > 1
                    else ""
                )
                L.append(
                    f"<summary>机制卡正文：{Path(path_s).name}（来自 tool-ops-tune）{share_note}</summary>"
                )
                L.append("")
                L.append(content.strip())
                L.append("")
                L.append("</details>")
            L.append("")
        if len(cards) > 1:
            L.append(
                "**实施顺序**：按 severity（critical → moderate → light）排序；"
                "互斥规则与叠加顺序见 tool-ops-tune ops-mechanism index.md。"
            )
            L.append("")
    else:
        L.append(
            "无命中机制卡（未命中瓶颈规则或数据不足）——机制路由的前提是先有 verdict 证据，"
            "禁止先看机制猜瓶颈。"
        )
        L.append("")

    # ---- ⑤ 数据缺口 ----
    L.append("## 5. 数据缺口与补采建议")
    L.append("")
    missing = facts.get("missing") or []
    if missing:
        for m in missing:
            L.append(f"- ⚠️ **{m.get('metric')}**：{m.get('reason')}")
        L.append("")
        L.append("补采建议：")
        if b.get("source_tool") == "npu-compute":
            L.append("```bash")
            L.append(
                "npu-compute --set full <program>   # full = 8 Section 全量（含 ResourceConflictRatio）"
            )
            L.append("```")
        else:
            L.append("```bash")
            L.append(
                "msopprof --application=<exe> --aic-metrics=PipeUtilization,Memory,L2Cache,ArithmeticUtilization,ResourceConflictRatio --output=<dir>"
            )
            L.append("```")
        L.append("")
    else:
        L.append("无已知数据缺口。")
        L.append("")
    return "\n".join(L) + "\n"


# ============================================================ HTML 渲染

CSS = """
:root {
  --surface: #fcfcfb; --page: #f9f9f7; --ink: #0b0b0b; --ink2: #52514e;
  --muted: #898781; --grid: #e1e0d9; --baseline: #c3c2b7;
  --border: rgba(11,11,11,0.10);
  --good: #0ca30c; --warn: #fab219; --critical: #d03b3b;
  --seq-1: #cde2fb; --seq-2: #9ec5f4; --seq-3: #6da7ec; --seq-4: #3987e5;
  --seq-5: #256abf; --seq-6: #184f95; --seq-7: #0d366b;
}
@media (prefers-color-scheme: dark) {
  :root { color-scheme: dark;
    --surface: #1a1a19; --page: #0d0d0d; --ink: #ffffff; --ink2: #c3c2b7;
    --grid: #2c2c2a; --baseline: #383835; --border: rgba(255,255,255,0.10);
  }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--page); color: var(--ink);
  font: 14px/1.65 system-ui, -apple-system, "Segoe UI", "Noto Sans SC", sans-serif;
  padding: 32px 16px 64px; }
.wrap { max-width: 980px; margin: 0 auto; }
header { border-bottom: 1px solid var(--border); padding-bottom: 16px; margin-bottom: 24px; }
h1 { font-size: 22px; margin: 0 0 6px; font-weight: 650; }
.sub { color: var(--ink2); font-size: 12.5px; }
h2 { font-size: 17px; margin: 36px 0 12px; font-weight: 650;
  padding-top: 18px; border-top: 1px solid var(--border); }
h3 { font-size: 14.5px; margin: 20px 0 8px; font-weight: 650; }
.card { background: var(--surface); border: 1px solid var(--border);
  border-radius: 10px; padding: 16px 18px; margin: 12px 0; }
table { border-collapse: collapse; width: 100%; margin: 10px 0; font-size: 13px; }
th { text-align: left; color: var(--ink2); font-weight: 600; font-size: 12px;
  padding: 7px 10px; border-bottom: 1px solid var(--baseline); }
td { padding: 7px 10px; border-bottom: 1px solid var(--grid); vertical-align: top; }
tr:last-child td { border-bottom: none; }
td.num, th.num { text-align: right; font-variant-numeric: tabular-nums; }
code { background: var(--grid); border-radius: 4px; padding: 1px 5px;
  font-size: 12px; font-family: ui-monospace, "SF Mono", Menlo, Consolas, monospace; }
pre { background: var(--surface); border: 1px solid var(--border); border-radius: 8px;
  padding: 12px 14px; overflow-x: auto; font-size: 12px; line-height: 1.55; }
.badge { display: inline-flex; align-items: center; gap: 5px; border-radius: 999px;
  padding: 2px 10px; font-size: 12px; font-weight: 600; }
.badge::before { content: ""; width: 8px; height: 8px; border-radius: 50%; background: currentColor; }
.b-critical { color: var(--critical); background: rgba(208,59,59,.10); }
.b-moderate { color: #b07800; background: rgba(250,178,25,.12); }
.b-light { color: var(--good); background: rgba(12,163,12,.10); }
.b-info { color: var(--seq-4); background: rgba(42,120,214,.10); }
.kpi-row { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
  gap: 12px; margin: 14px 0; }
.kpi { background: var(--surface); border: 1px solid var(--border); border-radius: 10px;
  padding: 12px 14px; }
.kpi .lab { color: var(--ink2); font-size: 11.5px; }
.kpi .val { font-size: 24px; font-weight: 650; margin-top: 2px; }
.kpi .un { color: var(--muted); font-size: 12px; margin-left: 3px; }
.kpi .note { color: var(--muted); font-size: 11.5px; margin-top: 2px; }
.bar-row { display: grid; grid-template-columns: 150px 1fr 120px; gap: 10px;
  align-items: center; margin: 7px 0; }
.bar-row .nm { font-size: 12.5px; color: var(--ink2); text-align: right; }
.bar-track { background: var(--grid); border-radius: 4px; height: 18px; position: relative; }
.bar-fill { height: 100%; border-radius: 4px 3px 3px 4px; min-width: 2px; position: relative; }
.bar-row .v { font-size: 12.5px; font-variant-numeric: tabular-nums; }
.legend { display: flex; flex-wrap: wrap; gap: 14px; margin: 10px 0 4px; font-size: 12px; color: var(--ink2); }
.legend .it { display: inline-flex; align-items: center; gap: 6px; }
.legend .sw { width: 10px; height: 10px; border-radius: 3px; }
details { margin: 8px 0; }
details summary { cursor: pointer; color: var(--seq-4); font-size: 13px; font-weight: 600;
  padding: 6px 0; user-select: none; }
details[open] summary { color: var(--ink2); }
.muted { color: var(--muted); }
.foot { color: var(--muted); font-size: 11.5px; margin-top: 40px;
  border-top: 1px solid var(--border); padding-top: 12px; }
.warnbox { border-left: 3px solid var(--warn); background: rgba(250,178,25,.07);
  padding: 10px 14px; border-radius: 0 8px 8px 0; margin: 10px 0; font-size: 13px; }
.gapbox { border-left: 3px solid var(--warn); }
@media (max-width: 640px) { .bar-row { grid-template-columns: 110px 1fr 90px; } }
"""


def seq_color(pct: float) -> str:
    """单蓝色阶（sequential）：占用越高越深（ordinal ramp，≥step 250 保对比度）。"""
    if pct >= 85:
        return "var(--seq-7)"
    if pct >= 70:
        return "var(--seq-6)"
    if pct >= 55:
        return "var(--seq-5)"
    if pct >= 40:
        return "var(--seq-4)"
    if pct >= 25:
        return "var(--seq-3)"
    if pct >= 10:
        return "var(--seq-2)"
    return "var(--seq-1)"


def render_html(b: dict) -> str:
    facts, verdict = b["facts"], b["verdict"]
    op = facts.get("op") or {}
    hw = facts.get("hardware") or {}
    stats = facts.get("block_stats") or {}
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    H = []
    H.append("<!DOCTYPE html>")
    H.append('<html lang="zh"><head><meta charset="utf-8">')
    H.append('<meta name="viewport" content="width=device-width, initial-scale=1">')
    H.append(f"<title>性能诊断 · {esc(op.get('Op Name', 'NPU'))}</title>")
    H.append(f"<style>{CSS}</style></head><body><div class='wrap'>")

    # header + KPI
    dev = hw.get("Device Info", {})
    chip = facts.get("chip") or dev.get("chip info", "—")
    H.append("<header>")
    H.append("<h1>NPU 算子性能诊断报告</h1>")
    H.append(
        f"<div class='sub'>{esc(chip)}"
        f"{' / ' + esc(facts.get('arch')) if facts.get('arch') else ''}"
        f" · <code>{esc(op.get('Op Name', '—'))}</code>"
        f" · {esc(op.get('Op Type', '—'))} 型 · {now} · "
        f"{'npu-compute' if b.get('source_tool') == 'npu-compute' else 'msopprof'} 采集"
        f" · 判定引擎 tool-ops-tune ops-verdict</div>"
    )
    H.append("</header>")

    v = verdict.get("verdict")
    v_badge = {
        "NEEDS_OPTIMIZATION": "b-critical",
        "NEAR_LIMIT": "b-light",
        "NO_BOUND_FOUND": "b-light",
        "INSUFFICIENT_DATA": "b-moderate",
    }.get(v, "b-info")
    scal = facts.get("scalars") or {}
    H.append(
        f"<div class='card'><div style='display:flex;align-items:center;gap:12px;flex-wrap:wrap'>"
        f"<span class='badge {v_badge}'>{VERDICT_ZH.get(v, v)}</span>"
        f"<span style='font-weight:600'>{esc(verdict.get('headline', ''))}</span></div>"
        f"<div class='muted' style='margin-top:6px;font-size:12.5px'>单次采集（single-shot）——结论为非稳态；判定只出自规则引擎（阈值溯源见 verdict.json），本报告只解释不判定。</div></div>"
    )

    H.append("<div class='kpi-row'>")

    def kpi(lab, val, un="", note=""):
        n = f"<div class='note'>{esc(note)}</div>" if note else ""
        return (
            f"<div class='kpi'><div class='lab'>{esc(lab)}</div>"
            f"<div class='val'>{esc(val)}<span class='un'>{esc(un)}</span></div>{n}</div>"
        )

    H.append(kpi("Task Duration", fmt(op.get("Task Duration(us)")), "µs"))
    H.append(
        kpi(
            "Block Dim",
            fmt(op.get("Block Dim")),
            "核",
            f"物理 {fmt(scal.get('physical_cores'))}",
        )
    )
    if stats.get("max_pipe"):
        mp = stats["max_pipe"]
        mpv = stats.get(f"{mp}_ratio_pct", {}).get("mean")
        H.append(kpi("主导流水", f"{mpv}", "%", PIPE_ZH.get(mp, mp)))
    if scal.get("gm_bw_usage_rate") is not None:
        H.append(
            kpi("GM 带宽利用率", fmt(scal.get("gm_bw_usage_rate")), "%", "Σ块口径")
        )
    if facts.get("pcsampling"):
        pcs = facts["pcsampling"]
        H.append(
            kpi(
                "Top Stall",
                fmt(pcs["top_stall_pct"]),
                "%",
                STALL_ZH.get(pcs["top_stall"], pcs["top_stall"]),
            )
        )
    if op.get("Current Freq") is not None and op.get("Rated Freq") is not None:
        try:
            cur, rated = float(op["Current Freq"]), float(op["Rated Freq"])
            note = "⚠️ 降频采集" if cur < rated * 0.9 else f"额定 {fmt(rated, 1)}"
            H.append(kpi("主频", fmt(cur, 1), "MHz", note))
        except (TypeError, ValueError):
            pass
    H.append("</div>")

    # ---- ② 瓶颈判定 ----
    H.append("<h2>2. 瓶颈判定（问题）</h2>")
    findings = [f for f in verdict.get("findings", []) if f.get("status") == "HIT"]
    findings.sort(key=lambda f: (SEV_ORDER.get(f.get("severity"), 3),))
    if findings:
        H.append(
            f"<div class='muted' style='font-size:12.5px;margin-bottom:8px'>"
            f"命中 {len(findings)} 项（L3 卡间 → L2 核间 → L1 单核 → L0 指令 分层瀑布，依序路由）</div>"
        )
        for i, f in enumerate(findings, 1):
            sev = f.get("severity", "light")
            H.append(
                f"<div class='card'><h3 style='margin:0 0 8px'>{i}. {esc(f.get('name_display') or f.get('name'))} "
                f"<span class='badge b-{sev}'>{STATUS_LABEL.get(sev, sev)}</span> "
                f"<span class='muted' style='font-weight:400;font-size:12px'>置信度 {esc(f.get('confidence', '—'))} · <code>{esc(f.get('rule_id'))}</code></span></h3>"
            )
            H.append("<table>")
            H.append(
                f"<tr><th>证据</th><td>{esc(f.get('evidence_display') or f.get('evidence', '—'))}</td></tr>"
            )
            if f.get("threshold_source"):
                H.append(
                    f"<tr><th>阈值溯源</th><td class='muted'>{esc(f['threshold_source'])}</td></tr>"
                )
            if f.get("severity_note"):
                H.append(
                    f"<tr><th>分级注记</th><td>{esc(f['severity_note'])}</td></tr>"
                )
            if f.get("speedup_range"):
                H.append(
                    f"<tr><th>典型收益区间</th><td>×{f['speedup_range'][0]}~{f['speedup_range'][1]}"
                    f" <span class='muted'>({esc(f.get('speedup_source', 'typical'))}；典型收益，非本算子预测)</span></td></tr>"
                )
            ces = f.get("counter_evidence") or []
            if ces:
                ce_html = ""
                for ce in ces:
                    note = ce.get("note", "")
                    if ce.get("unevaluated") and "反证指标缺失" not in note:
                        note += "（反证指标缺失，未评估）"
                    ce_html += f"<div>⚠️ {esc(note)}</div>"
                H.append(f"<tr><th>反证</th><td class='muted'>{ce_html}</td></tr>")
            H.append("</table></div>")
    else:
        H.append(f"<div class='card'>{esc(verdict.get('headline', ''))}</div>")
    watches = [
        (s.get("rule_id"), s.get("watch"))
        for s in verdict.get("skipped", [])
        if s.get("status") == "NOT_HIT" and s.get("watch")
    ]
    if watches:
        H.append("<div class='card'><b>观察档</b>（未达判定线，低成本先行项）<table>")
        for rid, note in watches:
            H.append(f"<tr><td><code>{esc(rid)}</code></td><td>{esc(note)}</td></tr>")
        H.append("</table></div>")

    # ---- ③ 证据 ----
    H.append("<h2>3. 证据（关键指标）</h2>")
    if stats and any(k.endswith("_ratio_pct") for k in stats):
        H.append(
            "<div class='card'><h3 style='margin-top:0'>逐 block 流水占用均值</h3>"
        )
        pipes = sorted(
            ((k[:-10], v) for k, v in stats.items() if k.endswith("_ratio_pct")),
            key=lambda kv: -kv[1]["mean"],
        )
        mx = max((v["mean"] for _, v in pipes), default=100) or 100
        scale = max(mx, 100)
        H.append("<div class='legend'>")
        for i, (name, v) in enumerate(pipes):
            H.append(
                f"<span class='it'><span class='sw' style='background:{seq_color(v['mean'])}'></span>"
                f"{esc(PIPE_ZH.get(name, name))}</span>"
            )
        H.append("</div>")
        H.append(
            "<table><tr><th>流水</th><th class='num'>均值</th><th class='num'>min</th><th class='num'>max</th><th style='width:38%'>占用</th></tr>"
        )
        for name, v in pipes:
            w = v["mean"] / scale * 100
            dom = " · 主导" if name == stats.get("max_pipe") else ""
            H.append(
                f"<tr><td>{esc(PIPE_ZH.get(name, name))}{dom}</td>"
                f"<td class='num'><b>{fmt(v['mean'], 1)}%</b></td>"
                f"<td class='num'>{fmt(v['min'], 1)}%</td>"
                f"<td class='num'>{fmt(v['max'], 1)}%</td>"
                f"<td><div class='bar-track'><div class='bar-fill' style='width:{w:.1f}%;background:{seq_color(v['mean'])}'></div></div></td></tr>"
            )
        H.append("</table>")
        ps = stats.get("pipe_sum_mean_pct")
        if ps is not None:
            is_msopprof = b.get("source_tool") == "msopprof"
            if 85 <= ps <= 115:
                interp = "串行无重叠指纹（Double Buffer 首选）"
            elif ps > 115:
                interp = (
                    "多 pipe 并行重叠（msopprof SIMT 口径常见）"
                    if is_msopprof
                    else "多 pipe 并行重叠（该算子标量与搬运/计算并行）"
                )
            else:
                interp = "pipe 间存在空泡"
            H.append(
                f"<div class='muted' style='font-size:12.5px'>pipe 占比求和 = <b>{fmt(ps, 1)}%</b> → {esc(interp)}"
                f" · 活跃 block {stats.get('blocks')} 个</div>"
            )
        if "time_us" in stats:
            t = stats["time_us"]
            H.append(
                f"<div class='muted' style='font-size:12.5px;margin-top:6px'>逐核耗时 mean {t['mean']} / max {t['max']} / min {t['min']} µs"
                f" · 长尾比 <b>{fmt(t.get('tail_ratio'))}</b> · 离散度 <b>{fmt(t.get('imbalance_pct'), 1)}%</b></div>"
            )
        H.append("</div>")
    pcs = facts.get("pcsampling")
    if pcs:
        H.append("<div class='card'><h3 style='margin-top:0'>PC 采样 stall 分布</h3>")
        H.append(
            f"<div class='muted' style='font-size:12.5px'>采样 {pcs['samples_total']} 样本 · "
            f"top：<b>{esc(STALL_ZH.get(pcs['top_stall'], pcs['top_stall']))} = {fmt(pcs['top_stall_pct'], 1)}%</b></div>"
        )
        dist = {k: v for k, v in pcs["distribution_pct"].items() if v >= 0.05}
        H.append(
            "<table><tr><th>stall 项</th><th class='num'>占比</th><th style='width:40%'>分布</th></tr>"
        )
        for k, v in sorted(dist.items(), key=lambda kv: -kv[1]):
            H.append(
                f"<tr><td>{esc(STALL_ZH.get(k, k))}</td><td class='num'>{fmt(v, 1)}%</td>"
                f"<td><div class='bar-track'><div class='bar-fill' style='width:{min(v, 100)}%;background:{seq_color(v)}'></div></div></td></tr>"
            )
        H.append("</table></div>")
    tr = facts.get("trace_summary")
    if tr and tr.get("sampled_lanes"):
        H.append(
            f"<div class='card'><h3 style='margin-top:0'>流水时间线采样</h3>"
            f"<div class='muted' style='font-size:12.5px'>PipeTrace 采样 {tr['sampled_lanes']} 条核轨道"
            f"（最多 6 个 Group，不代表全部核；不同轨道可重叠）</div><table>"
            f"<tr><th>流水</th><th class='num'>忙碌总量（µs，采样核）</th></tr>"
        )
        for k, v in tr.get("pipe_busy_total_us", {}).items():
            H.append(f"<tr><td>{esc(k)}</td><td class='num'>{fmt(v, 1)}</td></tr>")
        H.append("</table></div>")
    # 标量表
    scalar_rows = [
        (
            "Task Duration",
            scal.get("task_duration_us"),
            "µs",
            "OpInfoSummary/OpBasicInfo",
        ),
        ("Block Dim", scal.get("block_dim"), "", "OpInfoSummary/OpBasicInfo"),
        ("物理核数", scal.get("physical_cores"), "", "soc-spec reference"),
        ("ICache 缺失率", scal.get("icache_miss_rate"), "%", "PipeUtilization"),
        ("L2 命中率", scal.get("l2_hit_rate"), "%", "L2Cache"),
        (
            "MTE2 活跃带宽占峰值",
            scal.get("mte2_active_bw_pct_of_peak"),
            "%",
            "PipeUtilization × soc-spec",
        ),
        ("GM 带宽利用率", scal.get("gm_bw_usage_rate"), "%", "OpInfoSummary（Σ块）"),
        ("top stall 占比", scal.get("top_stall_pct"), "%", "summary.db PcSampling"),
    ]
    have = [r for r in scalar_rows if r[1] is not None]
    if have:
        H.append(
            "<div class='card'><h3 style='margin-top:0'>判定标量（verdict --scalar 输入）</h3><table>"
            "<tr><th>标量</th><th class='num'>值</th><th>来源</th></tr>"
        )
        for name, v, unit, src in have:
            H.append(
                f"<tr><td>{esc(name)}</td><td class='num'><b>{fmt(v)}{unit}</b></td>"
                f"<td class='muted'>{esc(src)}</td></tr>"
            )
        H.append("</table></div>")

    # ---- ④ 优化候选方案 ----
    H.append("<h2>4. 优化候选方案（机制路由）</h2>")
    cards = b.get("mechanism_cards") or []
    if cards:
        H.append(
            "<div class='muted' style='font-size:12.5px;margin-bottom:8px'>"
            "按 verdict 判定书 mechanism_refs 路由至 tool-ops-tune ops-mechanism 机制卡。"
            "单变量铁律：一次只实施一个机制。</div>"
        )
        seen_cards: dict[str, list[str]] = {}
        for c in cards:
            for cp in c.get("card_paths", []):
                seen_cards.setdefault(cp, []).append(c.get("rule_id", "?"))
        for i, c in enumerate(cards, 1):
            sev = c.get("severity", "light")
            H.append(
                f"<div class='card'><h3 style='margin:0 0 8px'>{i}. {esc(c.get('name'))} "
                f"<span class='badge b-{sev}'>{STATUS_LABEL.get(sev, sev)}</span></h3>"
            )
            H.append(
                f"<div style='font-size:13px'>判定规则 <code>{esc(c.get('rule_id'))}</code>"
                f" · 机制卡：{', '.join(f'<code>{esc(Path(p).name)}</code>' for p in c.get('card_paths', []))}"
                + (
                    f" · 典型收益 ×{c['speedup_range'][0]}~{c['speedup_range'][1]}"
                    if c.get("speedup_range")
                    else ""
                )
                + "</div>"
            )
            for path_s, content in zip(
                c.get("card_paths", []), c.get("card_contents", [])
            ):
                rules_sharing = seen_cards.get(path_s, [])
                if (
                    len(rules_sharing) > 1
                    and rules_sharing.index(c.get("rule_id", "?")) > 0
                ):
                    H.append(
                        f"<div class='muted' style='font-size:12px'>（{esc(Path(path_s).name)} 已在 {esc(rules_sharing[0])} 条目展开）</div>"
                    )
                    continue
                share_note = (
                    f"（同对应 {esc(', '.join(rules_sharing[1:]))}）"
                    if len(rules_sharing) > 1
                    else ""
                )
                H.append(
                    f"<details><summary>机制卡正文：{esc(Path(path_s).name)}（tool-ops-tune，八要素全量）{share_note}</summary>"
                )
                H.append(md_to_html(content))
                H.append("</details>")
            H.append("</div>")
    else:
        H.append(
            "<div class='card'>无命中机制卡——机制路由前提是先有 verdict 证据，禁止先看机制猜瓶颈。</div>"
        )

    # ---- ⑤ 数据缺口 ----
    H.append("<h2>5. 数据缺口与补采建议</h2>")
    missing = facts.get("missing") or []
    if missing:
        H.append(
            "<div class='card gapbox'><table><tr><th>缺失项</th><th>原因</th></tr>"
        )
        for m in missing:
            H.append(
                f"<tr><td><b>{esc(m.get('metric'))}</b></td><td>{esc(m.get('reason'))}</td></tr>"
            )
        H.append("</table>")
        if b.get("source_tool") == "npu-compute":
            cmd = "npu-compute --set full <program>"
        else:
            cmd = "msopprof --application=<exe> --aic-metrics=PipeUtilization,Memory,L2Cache,ArithmeticUtilization,ResourceConflictRatio --output=<dir>"
        H.append(
            f"<div style='margin-top:8px'>补采（full 采集覆盖全部指标组）：<pre>{esc(cmd)}</pre>"
            "<div class='muted' style='font-size:12px'>已有 manifest 的目录禁止重复采集；MC² 算子禁 host 侧计时。</div></div></div>"
        )
    else:
        H.append("<div class='card'>无已知数据缺口。</div>")

    H.append(
        f"<div class='foot'>tool-perf-analysis · 判定/机制/阈值均引自 tool-ops-tune"
        f"（ops-verdict 规则引擎 + ops-mechanism 机制卡）· 数据真源：facts.json / verdict.json ·"
        f" 生成于 {now}</div>"
    )
    H.append("</div></body></html>")
    return "\n".join(H)


def md_to_html(md_text: str) -> str:
    """机制卡 markdown → HTML（极简渲染：表/代码块/标题/列表；零依赖）。"""
    out, in_code, table_rows = [], False, []
    lines = md_text.splitlines()

    def flush_table():
        nonlocal table_rows
        if not table_rows:
            return ""
        head = table_rows[0]
        body = (
            [r for r in table_rows[2:]]
            if len(table_rows) > 1
            and set(table_rows[1].replace("|", "").replace("-", "").replace(" ", ""))
            == set()
            else table_rows[1:]
        )
        h = (
            "<table><tr>"
            + "".join(
                f"<th>{esc(c.strip())}</th>" for c in head.strip().strip("|").split("|")
            )
            + "</tr>"
        )
        for r in body:
            h += (
                "<tr>"
                + "".join(
                    f"<td>{esc(c.strip())}</td>"
                    for c in r.strip().strip("|").split("|")
                )
                + "</tr>"
            )
        h += "</table>"
        table_rows = []
        return h

    for line in lines:
        if line.strip().startswith("```"):
            out.append("</pre>" if in_code else "<pre>")
            in_code = not in_code
            continue
        if in_code:
            out.append(esc(line))
            continue
        s = line.strip()
        if s.startswith("|"):
            table_rows.append(s)
            continue
        if table_rows:
            out.append(flush_table())
        if not s:
            out.append("")
            continue
        if s.startswith("### "):
            out.append(f"<h3>{inline(s[4:])}</h3>")
        elif s.startswith("## "):
            out.append(f"<h3>{inline(s[3:])}</h3>")
        elif s.startswith("# "):
            out.append(f"<h3>{inline(s[2:])}</h3>")
        elif s.startswith("> "):
            out.append(f"<div class='warnbox'>{inline(s[2:])}</div>")
        elif s.startswith("- ") or s.startswith("* "):
            out.append(f"<div style='padding-left:14px'>· {inline(s[2:])}</div>")
        elif re.match(r"^\d+\. ", s):
            txt = re.sub(r"^\d+\. ", "", s)
            out.append(f"<div style='padding-left:14px'>· {inline(txt)}</div>")
        else:
            out.append(f"<p style='margin:4px 0'>{inline(s)}</p>")
    if table_rows:
        out.append(flush_table())
    if in_code:
        out.append("</pre>")
    return "\n".join(out)


def inline(s: str) -> str:
    s = esc(s)
    s = re.sub(r"`([^`]+)`", r"<code>\1</code>", s)
    s = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", s)
    return s


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bundle", required=True)
    ap.add_argument("--outdir", required=True)
    args = ap.parse_args()
    b = json.loads(Path(args.bundle).read_text(encoding="utf-8"))
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / "report.md").write_text(render_md(b), encoding="utf-8")
    (outdir / "report.html").write_text(render_html(b), encoding="utf-8")
    print(f"render: {outdir}/report.md")
    print(f"render: {outdir}/report.html")
    return 0


if __name__ == "__main__":
    import sys

    sys.exit(main())
