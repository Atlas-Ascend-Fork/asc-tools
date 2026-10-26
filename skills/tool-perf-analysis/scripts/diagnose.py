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
"""diagnose.py — tool-perf-analysis 主执行器（30 秒性能报告解读）

编排链（判定进脚本原则——本脚本只编排，判定/机制出自 tool-ops-tune）：

  ① extract_facts.py   .npu-rep（950）或 PROF_*（msopprof）→ facts.json（取数+标量）
  ② tool-ops-tune ops-verdict verdict.py
                        facts.scalar_str + PipeUtilization.csv → verdict.json
                        （分层规则引擎 L3→L2→L1→L0：Bound 判定 + 证据 + 反证 + 置信度）
  ③ mechanism 路由      verdict.findings[].mechanism_refs → tool-ops-tune
                        ops-mechanism 机制卡索引（references/optimize/index.md）
                        → 每命中一张候选方案卡（原理/前提/收益/风险/验证/回滚）
  ④ render_report.py    facts + verdict + 机制卡 → report.md + report.html

用法：
  python3 scripts/diagnose.py --rep <report.npu-rep> [-o <outdir>] [--soc a5]
  python3 scripts/diagnose.py --prof <PROF_dir>  [-o <outdir>] --soc a2
  可选：--ops-tune <tool-ops-tune 根目录>（缺省自动探测环境安装路径）
        --kernel-name（多 kernel 过滤）--max-findings N（候选方案上限，缺省 3）

耗时预算（30s 门禁）：解包 <1s；facts 抽取 <1s；verdict.py <1s；机制卡路由 <0.5s；
渲染 <1s —— 瓶颈在 LLM 解读而非脚本链，脚本链全程 <5s。
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
SKILL_ROOT = HERE.parent

# tool-ops-tune 安装路径探测序（GitCode asc-tools skills/ 下为 tool-ops-tune）：
#   ① 显式 --ops-tune；② 环境变量 OPS_TUNE_HOME；③ ~/.claude/skills/tool-ops-tune；
#   ④ 本仓同级 tool-ops-tune；⑤ 同级 asc-tools*/skills/tool-ops-tune
OPS_TUNE_CANDIDATES = [
    Path.home() / ".claude" / "skills" / "tool-ops-tune",
    SKILL_ROOT.parent / "tool-ops-tune",
]
OPS_TUNE_SUBDIR_CANDIDATES = ["ops-verdict", "ops-mechanism"]


def find_ops_tune(explicit: str | None) -> Path:
    """定位 tool-ops-tune。两种安装形态都支持：
    ① 套件形态：<root>/ops-verdict + <root>/ops-mechanism（GitCode asc-tools skills/tool-ops-tune）
    ② 平铺形态：<root> 本身即技能目录，同级有 ops-verdict / ops-mechanism
       （部分环境把五子域拆成独立 skill 安装到 ~/.claude/skills/）
    """
    cands = []
    if explicit:
        cands.append(Path(explicit))
    env = os.environ.get("OPS_TUNE_HOME")
    if env:
        cands.append(Path(env))
    cands.extend(OPS_TUNE_CANDIDATES)
    for p in sorted(SKILL_ROOT.parent.glob("asc-tools*/skills/tool-ops-tune")):
        cands.append(p)
    for p in sorted(SKILL_ROOT.parent.glob("skills/tool-ops-tune")):
        cands.append(p)
    # 平铺形态：本 skill 所在技能目录下的兄弟子域（~/.claude/skills/ops-* 拆装）
    siblings = [
        SKILL_ROOT.parent,
        Path.home() / ".claude" / "skills",
    ]
    for cand in cands:
        if all((cand / d).is_dir() for d in OPS_TUNE_SUBDIR_CANDIDATES):
            return cand
    for root in siblings:
        if all((root / d).is_dir() for d in OPS_TUNE_SUBDIR_CANDIDATES):
            return root
    found = "\n  ".join(str(c) for c in cands)
    raise SystemExit(
        "ERROR: 未找到 tool-ops-tune 套件（需含 ops-verdict 与 ops-mechanism 子域）。\n"
        f"  已探测:\n  {found}\n"
        "  安装方式见 https://gitcode.com/cann/asc-tools skills/tool-ops-tune，"
        "或用 --ops-tune <路径> 显式指定。"
    )


def run(cmd: list[str], label: str, t0: float) -> float:
    r = subprocess.run(cmd, capture_output=True, text=True)
    dt = time.time() - t0
    if r.returncode != 0:
        print(
            f"[{label}] FAILED (exit {r.returncode})\n{r.stdout}\n{r.stderr}",
            file=sys.stderr,
        )
        raise SystemExit(f"{label} 失败（exit {r.returncode}）")
    for line in r.stdout.splitlines():
        print(f"[{label}] {line}")
    return dt


def main() -> int:
    ap = argparse.ArgumentParser(description="tool-perf-analysis 性能报告解读主执行器")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--rep", help="npu-compute .npu-rep 报告（Ascend 950 / dav-3510）")
    g.add_argument("--prof", help="msopprof 采集目录（非 950 芯片）")
    ap.add_argument(
        "--soc",
        default=None,
        choices=["a2", "a3", "a5"],
        help="SoC（msopprof 源必填；npu-rep 源从 HardwareInfo 自动识别）",
    )
    ap.add_argument("--kernel-name", default=None)
    ap.add_argument(
        "--ops-tune", default=None, help="tool-ops-tune 根目录（缺省自动探测）"
    )
    ap.add_argument("--max-findings", type=int, default=3, help="候选方案上限")
    ap.add_argument(
        "-o", "--output-dir", default=None, help="输出目录（缺省 diagnose_out/<名字>）"
    )
    args = ap.parse_args()

    if args.prof and not args.soc:
        raise SystemExit(
            "ERROR: msopprof 源（--prof）必须显式 --soc a2|a3|a5"
            "（产物无 HardwareInfo，峰值标尺无法自动识别）"
        )

    t0 = time.time()
    ops_tune = find_ops_tune(args.ops_tune)
    print(f"[ops-tune] {ops_tune}")

    src = Path(args.rep) if args.rep else Path(args.prof)
    if not src.exists():
        raise SystemExit(f"ERROR: 输入不存在: {src}")
    name = src.stem or src.name

    outdir = Path(args.output_dir) if args.output_dir else Path("diagnose_out") / name
    outdir.mkdir(parents=True, exist_ok=True)

    # ① facts
    facts_path = outdir / "facts.json"
    cmd = [sys.executable, str(HERE / "extract_facts.py")]
    cmd += ["--rep", str(src)] if args.rep else ["--prof", str(src)]
    if args.soc:
        cmd += ["--soc", args.soc]
    if args.kernel_name:
        cmd += ["--kernel-name", args.kernel_name]
    cmd += [
        "--soc-spec",
        str(HERE.parent / "references" / "soc-spec.yaml"),
        "--workdir",
        str(outdir / "_unpacked"),
        "-o",
        str(facts_path),
    ]
    run(cmd, "facts", t0)
    facts = json.loads(facts_path.read_text(encoding="utf-8"))

    # ② verdict（判定只出自 tool-ops-tune 规则引擎）
    verdict_path = outdir / "verdict.json"
    # soc 未知（msopprof 源漏传 --soc，或 npu-compute 源 HardwareInfo 无芯片信息）时
    # fail loud：静默回落到任一 SoC 峰值标尺会让 A2/A5 判定错位（峰值/核数差异大）
    soc = facts.get("soc")
    if not soc:
        raise SystemExit(
            "ERROR: 无法确定 SoC——msopprof 源必须显式 --soc a2|a3|a5；"
            "npu-compute 源需报告含 HardwareInfo.jsonl（chip info）。"
            "不做静默回落（A2 与 A5 的 GM 峰值/核数标尺差异足以改变 Bound 结论）。"
        )
    rules = ops_tune / "ops-verdict" / "rules"
    vcmd = [
        sys.executable,
        str(ops_tune / "ops-verdict" / "scripts" / "verdict.py"),
        "--rules",
        ",".join(
            str(rules / r)
            for r in (
                "L3-inter-card.yaml",
                "L2-inter-core.yaml",
                "L1-single-core.yaml",
                "L0-instruction.yaml",
            )
        ),
        "--soc",
        soc,
        "-o",
        str(verdict_path),
    ]
    if args.kernel_name:
        vcmd += ["--kernel-name", args.kernel_name]
    pipe_csv = (facts.get("csv_paths") or {}).get("PipeUtilization")
    if pipe_csv and Path(pipe_csv).exists():
        vcmd += ["--csv", pipe_csv]
    if facts.get("scalar_str"):
        vcmd += ["--scalar", facts["scalar_str"]]
    else:
        # 无任何标量：不伪造数值标量送判定引擎（伪造 task_duration_us=0 会把
        # 「缺失」伪装成「实测 0」，掩盖本应出现的 INSUFFICIENT_DATA）
        print(
            "WARN: 无可用标量（OpBasicInfo/summary 均未产出），"
            "判定引擎将按绑定契约输出 INSUFFICIENT_DATA",
            file=sys.stderr,
        )
    run(vcmd, "verdict", t0)
    verdict = json.loads(verdict_path.read_text(encoding="utf-8"))

    # ③ 机制卡路由（verdict.findings[].mechanism_refs → ops-mechanism 机制卡）
    mech_index = ops_tune / "ops-mechanism" / "references" / "optimize" / "index.md"
    hits = [f for f in verdict.get("findings", []) if f.get("status") == "HIT"]

    # 人读化补强（不改判定，只改呈现）：L0_STALL_DOMINANT 的规则名是占位文案，
    # 用 PcSampling top_stall 枚举补全为「指令 stall 主导：<stall 语义>」
    pcs = facts.get("pcsampling") or {}
    stall_zh = {
        "scoreboard_not_ready": "访存依赖等待（scoreboard not ready）",
        "nop_cycles": "空转周期主导",
        "register_bank_conflict": "寄存器 Bank 冲突",
        "resource_conflict": "资源冲突",
        "warp_level_sync": "warp 级同步等待",
        "divergence_stack_spill": "分支发散栈溢出",
        "ibuf_empty": "取指饥饿",
        "others": "其他",
    }
    if pcs.get("top_stall"):
        for f in hits:
            if f.get("rule_id") == "L0_STALL_DOMINANT":
                zh = stall_zh.get(pcs["top_stall"], pcs["top_stall"])
                f["name_display"] = f"指令 stall 主导：{zh}"
                f["evidence_display"] = (
                    f"PC 采样 {pcs['samples_total']} 样本中 {pcs['top_stall']} 占 "
                    f"{pcs['top_stall_pct']}%（top_stall_pct={pcs['top_stall_pct']}）"
                )

    cards = []
    for f in hits[: args.max_findings]:
        refs = list(f.get("mechanism_refs") or [])
        # stall 类型补充路由（references/mechanism-routing.md 的 PcSampling 表）：
        # L0_STALL_DOMINANT 规则只挂了 pipeline.md；按 top_stall 语义补挂
        # data-copy.md（访存依赖）/ api-usage.md（取指/分支）等对应机制卡
        stall_extra = {
            "scoreboard_not_ready": ["ops-mechanism:references/optimize/data-copy.md"],
            "ibuf_empty": ["ops-mechanism:references/optimize/api-usage.md"],
            "divergence_stack_spill": [
                "ops-mechanism:references/optimize/api-usage.md"
            ],
            "register_bank_conflict": [
                "ops-mechanism:references/optimize/ascendc-regbase-vf.md"
            ],
        }
        if f.get("rule_id") == "L0_STALL_DOMINANT" and pcs.get("top_stall"):
            for extra in stall_extra.get(pcs["top_stall"], []):
                if extra not in refs:
                    refs.append(extra)
        card_paths = []
        for ref in refs:
            # ref 形如 "ops-mechanism:references/optimize/data-copy.md"：
            # 前缀 ops-mechanism: 对应 tool-ops-tune 仓库 skills/tool-ops-tune/ops-mechanism/
            # 子域目录（GitCode asc-tools 安装形态），即 <ops-tune>/ops-mechanism/<rel>
            rel = ref.split(":", 1)[1] if ":" in ref else ref
            rel = rel.lstrip("/")
            for cand in (
                ops_tune / "ops-mechanism" / rel,
                ops_tune
                / rel,  # 已安装到 ~/.claude/skills/ops-mechanism 的平铺形态兜底
            ):
                if cand.exists():
                    card_paths.append(cand)
                    break
        cards.append(
            {
                "rule_id": f.get("rule_id"),
                "name": f.get("name_display") or f.get("name"),
                "severity": f.get("severity"),
                "confidence": f.get("confidence"),
                "speedup_range": f.get("speedup_range"),
                "mechanism_refs": refs,
                "card_paths": [str(p) for p in card_paths],
                "card_contents": [p.read_text(encoding="utf-8") for p in card_paths],
            }
        )

    bundle = {
        "schema": "opstune-perf-analysis.bundle/1.0",
        "generated_at": verdict.get("generated_at"),
        "source_tool": facts.get("source_tool"),
        "source_path": facts.get("source_path"),
        "soc": facts.get("soc"),
        "facts": facts,
        "verdict": verdict,
        "mechanism_cards": cards,
        "mechanism_index_path": str(mech_index) if mech_index.exists() else None,
        "ops_tune_root": str(ops_tune),
    }
    bundle_path = outdir / "diagnose_bundle.json"
    bundle_path.write_text(
        json.dumps(bundle, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )

    # ④ 渲染（md + html）
    rcmd = [
        sys.executable,
        str(HERE / "render_report.py"),
        "--bundle",
        str(bundle_path),
        "--outdir",
        str(outdir),
    ]
    run(rcmd, "render", t0)

    elapsed = time.time() - t0
    print(f"[done] {elapsed:.2f}s  → {outdir}/report.md + {outdir}/report.html")
    return 0


if __name__ == "__main__":
    sys.exit(main())
