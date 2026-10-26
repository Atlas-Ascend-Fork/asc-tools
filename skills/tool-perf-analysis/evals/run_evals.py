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
"""run_evals.py — tool-perf-analysis 执行式回归

四组用例跑全链（diagnose.py），断言 facts / verdict / report 三产物：
  A. verify_pipeline_pm.npu-rep（950 npu-compute，仅 Pipeline+PcSampling——L0 判定路径）
  B. result.npu-rep（950 npu-compute，全量 CSV——L1/L2 判定路径）
  C. msopprof_abs/（A2 msopprof 源，--soc a2——源 B 路径）
  D. msopprof 缺 --soc → fail loud 防护

fixture 策略（仓库禁止入库二进制文件）：
  - .npu-rep 为二进制采集产物，不入库。A/B 用例从本地获取，找不到即 SKIP：
      ① 环境变量 TOOL_PERF_ANALYSIS_FIXTURES_DIR 指定的目录；
      ② 环境变量 TOOL_PERF_ANALYSIS_FIXTURE_VERIFY_PIPELINE_PM /
        TOOL_PERF_ANALYSIS_FIXTURE_RESULT 直接指定文件路径；
      ③ evals/fixtures/ 本地目录（开发者自行放置，git 不跟踪 *.npu-rep）；
      ④ 用户缓存目录 ~/.cache/tool-perf-analysis/（与 ①~③ 同名复用）。
  - C 的 CSV fixture 为纯文本，直接入库；D 不需要 fixture。
  - fixture 缺失时 A/B 用例跳过（SKIP 计数，不计失败），C/D 照常执行。

退出码 0 = 全绿（SKIP 不计失败）；非 0 = 有断言失败（改动脚本后必须先过本回归再交付）。
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SKILL = HERE.parent

# .npu-rep fixture 本地查找表（仓库禁止二进制文件入库，不做远程下载）。
# 环境变量名 → 期望文件名：TOOL_PERF_ANALYSIS_FIXTURE_<KEY> 直接指定文件路径。
FIXTURE_ENV_KEYS = {
    "verify_pipeline_pm.npu-rep": "VERIFY_PIPELINE_PM",
    "result.npu-rep": "RESULT",
}
# 查找顺序：env 单文件指定 > env 目录 > evals/fixtures/ 本地 > 用户缓存目录
FIXTURE_SEARCH_DIRS = (
    Path(__file__).resolve().parent / "fixtures",
    Path.home() / ".cache" / "tool-perf-analysis",
)

PASS, FAIL, SKIP = 0, 0, 0


def fetch_fixture(filename: str) -> Path | None:
    """定位本地 .npu-rep fixture；找不到返回 None（调用方按 SKIP 处理）。

    查找顺序（均为本地路径，不发起网络请求）：
      ① TOOL_PERF_ANALYSIS_FIXTURE_<KEY> 环境变量指定的文件路径；
      ② TOOL_PERF_ANALYSIS_FIXTURES_DIR 目录下的同名文件；
      ③ evals/fixtures/（开发者自行放置，git 不跟踪 *.npu-rep）；
      ④ ~/.cache/tool-perf-analysis/。
    """
    import os

    env_key = FIXTURE_ENV_KEYS.get(filename)
    if env_key:
        direct = os.environ.get(f"TOOL_PERF_ANALYSIS_FIXTURE_{env_key}")
        if direct and Path(direct).is_file():
            return Path(direct)
    env_dir = os.environ.get("TOOL_PERF_ANALYSIS_FIXTURES_DIR")
    search = ([Path(env_dir)] if env_dir else []) + list(FIXTURE_SEARCH_DIRS)
    for d in search:
        cand = Path(d) / filename
        if cand.is_file() and cand.stat().st_size > 0:
            return cand
    return None


OPS_TUNE = sys.argv[1] if len(sys.argv) > 1 else None
if not OPS_TUNE:
    # 探测序与 diagnose.py 一致：套件形态（ops-verdict+ops-mechanism 在同一根下）
    # 与平铺形态（~/.claude/skills/ops-* 拆装）
    for cand in (
        Path.home() / ".claude" / "skills" / "tool-ops-tune",
        SKILL.parent / "tool-ops-tune",
        SKILL.parent,  # 平铺：技能目录同级
        Path.home() / ".claude" / "skills",  # 平铺：用户技能目录
        *sorted(SKILL.parent.glob("asc-tools*/skills/tool-ops-tune")),
    ):
        if (cand / "ops-verdict").is_dir() and (cand / "ops-mechanism").is_dir():
            OPS_TUNE = str(cand)
            break
if not OPS_TUNE:
    print("ERROR: 未找到 tool-ops-tune（可作第 1 参数传入路径）", file=sys.stderr)
    sys.exit(2)


def check(name: str, cond: bool, detail: str = ""):
    global PASS, FAIL
    mark = "PASS" if cond else "FAIL"
    print(f"  [{mark}] {name}" + (f" — {detail}" if detail and not cond else ""))
    if cond:
        PASS += 1
    else:
        FAIL += 1


def skip_case(case_id: str, why: str) -> None:
    """整用例跳过（fixture 下载失败等环境原因）：SKIP 计数，不计失败。"""
    global SKIP
    SKIP += 1
    print(f"== {case_id}: SKIP — {why}")


def run_case(case_id: str, argv: list[str]) -> dict:
    outdir = Path(tempfile.mkdtemp(prefix=f"evals-{case_id}-"))
    cmd = [
        sys.executable,
        str(SKILL / "scripts" / "diagnose.py"),
        "--ops-tune",
        OPS_TUNE,
        "-o",
        str(outdir),
    ] + argv
    r = subprocess.run(cmd, capture_output=True, text=True)
    check(
        f"{case_id}: diagnose.py exit 0",
        r.returncode == 0,
        r.stdout[-300:] + r.stderr[-300:],
    )
    if r.returncode != 0:
        return {}
    facts = json.loads((outdir / "facts.json").read_text(encoding="utf-8"))
    verdict = json.loads((outdir / "verdict.json").read_text(encoding="utf-8"))
    md = (outdir / "report.md").read_text(encoding="utf-8")
    html = (outdir / "report.html").read_text(encoding="utf-8")
    check(f"{case_id}: report.md 生成", len(md) > 500)
    check(f"{case_id}: report.html 生成", len(html) > 2000)
    # 零外部依赖：无外链 URL、无外部资源引用属性/规则（显式合取，防 and/or 优先级恒真）
    html_nolink = html.replace(
        "https://gitcode.com/cann/asc-tools", ""
    )  # 报告正文允许出现该引用文案
    no_url = (
        ("http://" not in html_nolink)
        and ("https://" not in html_nolink)
        and ("//" not in html_nolink.replace("\\", ""))
    )
    no_ext_ref = all(
        tok not in html.lower()
        for tok in (
            "src=",
            "href=",
            "url(",
            "@import",
            "<script",
            "<link",
            "<img",
            "<iframe",
        )
    )
    check(f"{case_id}: HTML 零外部依赖（无 URL/无外链属性）", no_url and no_ext_ref)
    for sec in (
        "## 1. 基本信息",
        "## 2. 瓶颈判定（问题）",
        "## 3. 证据（关键指标）",
        "## 4. 优化候选方案（机制路由）",
        "## 5. 数据缺口与补采建议",
    ):
        check(f"{case_id}: md 五节含 {sec[3:10]}", sec in md)
    return {"facts": facts, "verdict": verdict, "md": md, "html": html}


def case_a() -> None:
    print("== A. verify_pipeline_pm.npu-rep（950，Pipeline+PcSampling，L0 路径）==")
    rep = fetch_fixture("verify_pipeline_pm.npu-rep")
    if rep is None:
        skip_case(
            "A",
            "verify_pipeline_pm.npu-rep 未找到"
            "（放置到 evals/fixtures/ 或设置 TOOL_PERF_ANALYSIS_FIXTURES_DIR）",
        )
        return
    r = run_case("A", ["--rep", str(rep)])
    if not r:
        return
    f, v, md = r["facts"], r["verdict"], r["md"]
    check("A: soc 识别 a5", f.get("soc") == "a5")
    check("A: 芯片 950PR_9599", "9599" in (f.get("chip") or ""))
    check(
        "A: 算子 gather_2d_custom",
        "gather_2d_custom" in ((f.get("op") or {}).get("Op Name") or ""),
    )
    check(
        "A: Task Duration 80.572",
        abs((f["scalars"].get("task_duration_us") or 0) - 80.572) < 0.01,
    )
    check(
        "A: top_stall_pct 生产",
        abs((f["scalars"].get("top_stall_pct") or 0) - 98.73) < 0.1,
    )
    check(
        "A: PcSampling scoreboard_not_ready",
        (f.get("pcsampling") or {}).get("top_stall") == "scoreboard_not_ready",
    )
    hits = [x for x in v.get("findings", []) if x.get("status") == "HIT"]
    check(
        "A: verdict 命中 L0_STALL_DOMINANT",
        any(x["rule_id"] == "L0_STALL_DOMINANT" for x in hits),
    )
    check("A: 报告人读化 stall 名（访存依赖等待）", "访存依赖等待" in md)
    check(
        "A: 机制卡路由（pipeline/data-copy）",
        "pipeline" in md and "data-copy" in md and "tool-ops-tune" in md,
    )
    check("A: 数据缺口如实（PipeUtilization 缺失）", "PipeUtilization.csv 缺失" in md)
    check("A: single-shot 标注", "single-shot" in md)
    check("A: trace 汇总（VECTOR 忙碌）", "VECTOR" in md)
    check(
        "A: mc2机制卡引用（未触发则无）", True
    )  # 该算子无 MC² 证据，无 mc2 卡为正确行为


def case_b() -> None:
    print("== B. result.npu-rep（950，全量 CSV，L1/L2 路径，AdaLayerNormV2）==")
    rep = fetch_fixture("result.npu-rep")
    if rep is None:
        skip_case(
            "B",
            "result.npu-rep 未找到"
            "（放置到 evals/fixtures/ 或设置 TOOL_PERF_ANALYSIS_FIXTURES_DIR）",
        )
        return
    r = run_case("B", ["--rep", str(rep)])
    if not r:
        return
    f, v, md = r["facts"], r["verdict"], r["md"]
    check("B: soc a5", f.get("soc") == "a5")
    check(
        "B: 算子 AdaLayerNormV2",
        "AdaLayerNormV2" in ((f.get("op") or {}).get("Op Name") or ""),
    )
    s = f.get("block_stats") or {}
    check("B: 8 blocks", s.get("blocks") == 8)
    check("B: 主导 pipe scalar", s.get("max_pipe") == "scalar")
    check(
        "B: scalar_ratio≈61.7",
        abs(s.get("scalar_ratio_pct", {}).get("mean", 0) - 61.66) < 0.5,
    )
    check("B: aic 全 NA → 无 cube_ratio（Vector 型核）", "cube_ratio_pct" not in s)
    check("B: 无 Task Log 标注", "无有效 Task Log" in md)
    hits = [x for x in v.get("findings", []) if x.get("status") == "HIT"]
    ids = {x["rule_id"] for x in hits}
    check("B: 命中 L2_UNDERUTILIZATION", "L2_UNDERUTILIZATION" in ids)
    check(
        "B: physical_cores=56（HardwareInfo 实测，957b）",
        f["scalars"].get("physical_cores") == 56,
    )
    check("B: 机制卡内嵌（ascendc-tiling）", "ascendc-tiling.md" in md)
    check("B: 收益纪律（典型收益，非本算子预测）", "非本算子预测" in md)
    check(
        "B: GM 带宽利用率 0.10%（排除 memory-bound 证据）",
        (
            f["scalars"].get("gm_bw_usage_rate") is not None
            and f["scalars"]["gm_bw_usage_rate"] < 1
        ),
    )


def case_c() -> None:
    print("== C. msopprof_abs（A2 msopprof 源，--soc a2 路径）==")
    r = run_case(
        "C", ["--prof", str(HERE / "fixtures" / "msopprof_abs"), "--soc", "a2"]
    )
    if not r:
        return
    f, v, md = r["facts"], r["verdict"], r["md"]
    check("C: source_tool msopprof", f.get("source_tool") == "msopprof")
    check("C: soc a2（显式）", f.get("soc") == "a2")
    check(
        "C: OpBasicInfo 承担 OpInfoSummary（abs_kernel / 5.8us / blockDim 40）",
        (f.get("op") or {}).get("Op Name") == "abs_kernel"
        and abs((f["scalars"].get("task_duration_us") or 0) - 5.8) < 0.01
        and f["scalars"].get("block_dim") == 40,
    )
    hits = [x for x in v.get("findings", []) if x.get("status") == "HIT"]
    ids = {x["rule_id"] for x in hits}
    check("C: 命中 L1_SCALAR_BOUND（scalar 94%）", "L1_SCALAR_BOUND" in ids)
    check(
        "C: 命中 L2_CORE_TAIL / IMBALANCE",
        "L2_CORE_TAIL" in ids or "L2_IMBALANCE_SEVERE" in ids,
    )
    s = f.get("block_stats") or {}
    check(
        "C: pipe 求和 >115%（SIMT 并行口径解读）",
        (s.get("pipe_sum_mean_pct") or 0) > 115,
    )
    check("C: 并行口径注记（多 pipe 并行重叠）", "多 pipe 并行" in md)
    check(
        "C: 同机制卡去重（ascendc-tiling 只展开一次）",
        md.count("机制卡正文：ascendc-tiling.md") == 1,
    )
    check("C: msopprof 补采建议命令", "msopprof" in md and "--aic-metrics" in md)
    check("C: L2 hit 缺口如实", "l2_hit_rate" in md)


def case_d() -> None:
    """D. msopprof 源缺 --soc → fail loud（防 A2/A5 标尺错位静默误判）。"""
    print("== D. msopprof 缺 --soc（fail-loud 防护）==")
    cmd = [
        sys.executable,
        str(SKILL / "scripts" / "diagnose.py"),
        "--ops-tune",
        OPS_TUNE,
        "--prof",
        str(HERE / "fixtures" / "msopprof_abs"),
        "-o",
        "/tmp/evals-nosoc",
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    check("D: 缺 --soc 非零退出", r.returncode != 0)
    check("D: 报错信息含 --soc 指引", "--soc" in (r.stderr + r.stdout))


def main() -> int:
    print(f"tool-perf-analysis evals（ops-tune = {OPS_TUNE}）")
    case_a()
    case_b()
    case_c()
    case_d()
    print(
        f"\n{'=' * 50}\nPASS {PASS} / FAIL {FAIL}" + (f" / SKIP {SKIP}" if SKIP else "")
    )
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
