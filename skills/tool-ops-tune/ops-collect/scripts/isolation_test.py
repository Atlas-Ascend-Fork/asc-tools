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
"""isolation_test.py — MC² 隔离测试编排（T_comm / T_compute 采集 → R 值）

设计对齐（docs/算子调优AGENT设计文档.md §4.3 数据集模型）：
  - MC² 隔离测试方法论此前长期停留在文档形态（复制源码、注释 process、tileCnt=1、
    多 rank 采集、算 R 值全靠人工执行）—— 本脚本把它变成可重复的实验编排。
  - 纪律继承：隔离测试在源码副本进行（不碰用户原目录）；多 rank 并行采集；
    循环 ≥10 次前 5 warm-up；跨 rank 取 max（木桶效应）。
  - 输出直接可喂 verdict.py：--scalar t_comm_us=...,t_compute_us=...

用法（典型，宿主程序须支持 COMM_ONLY/COMPUTE_ONLY 环境变量开关或源码注释由 Agent 完成）：
  python3 isolation_test.py --exe ./build/mc2_demo --args "4096 4096 4096 4 perf 1" \\
      --prof-dir ./prof/iso --output iso_result.json

输出 iso_result.json：
  { t_comm_us, t_compute_us, r_value, bound_hint, rank_detail: {...}, steady_rule: "10轮前5warmup后5均值，跨rank取max" }

R 值档位真源：ops-verdict 规则包 L3-inter-card.yaml 的 r_value 叶子条件（脚本启动时解析，
解析失败即报错退出——fail loud，保证规则演进时脚本不会静默回退到旧常量）。
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import subprocess
import sys
from pathlib import Path

WARMUP = 5  # 前 5 次 warm-up（L2 预热、DVFS 爬升）
LOOPS = 10  # 循环至少 10 次
# MTE2 占比候选列（与 L1 词汇表 mte2_ratio 同源，三代列名兼容）
MTE2_RATIO_COLUMNS = [
    "aic_mte2_ratio",
    "aiv_mte2_ratio",
    "ai_mte2_ratio",
    "Mte2Utilization(%)",
    "mte2_ratio",
    "MTE2(%)",
]

# L3 规则包默认路径（相对本脚本定位，仓内布局免传参；--rules 可覆写）
L3_RULES_FILE = "L3-inter-card" + ".yaml"  # MC² 卡间判定规则包
DEFAULT_L3_RULES = (
    Path(__file__).resolve().parents[2] / "ops-verdict" / "rules" / L3_RULES_FILE
)


def r_bands_from_rules(rules_path: str) -> list[tuple]:
    """从 L3 规则包 YAML 派生 R 值档位表（规则 YAML 是唯一真源）。

    解析每条以 r_value 为唯一 metric 叶子条件、区间形状完整的规则，产出
    [(lo, hi, rule_id, name), ...] 按 hi 升序（lo=None 表示无下界）。
    解析不出任何档位 → 直接抛错退出（fail loud）。
    """
    import yaml

    pack = yaml.safe_load(Path(rules_path).read_text(encoding="utf-8"))
    bands = []
    for rule in pack.get("rules", []):
        leaves = _collect_metric_leaves(rule.get("condition"), "r_value")
        if not leaves:
            continue
        lo = hi = None
        ok = True
        for leaf in leaves:
            op, val = leaf["op"], leaf["value"]
            if op == "between" and isinstance(val, list) and len(val) == 2:
                lo, hi = float(val[0]), float(val[1])
            elif isinstance(val, (int, float)):
                if op in ("gt", "gte") and lo is None:
                    lo = float(val)
                elif op in ("lt", "lte") and hi is None:
                    hi = float(val)
                else:
                    ok = False
                    break
            else:
                ok = False
                break
        if ok and (lo is not None or hi is not None):
            # 单叶 gt（无上界，如 STRONG_COMPUTE R>2）：hi 置 inf，作为兜底档
            bands.append(
                (
                    lo,
                    hi if hi is not None else float("inf"),
                    rule["id"],
                    rule.get("name", rule["id"]),
                )
            )
    if not bands:
        raise RuntimeError(
            f"无法从 {rules_path} 解析出 r_value 档位（规则结构变化？fail loud 退出）"
        )
    return sorted(bands, key=lambda b: b[1])


def classify_r(r: float, bands: list[tuple]) -> tuple[str, str]:
    """按派生档位分类 R 值：唯一命中档 = 唯一满足区间条件的档；多重命中取最紧。

    边界对齐规则表语义：STRONG_COMM 为开区间上界（R<hi），其余档下界 gte /
    上界 lte/between——r 恰落档界（R=0.5/0.9/1.1/2.0）时同时满足两档的
    "r ≤ hi"宽松判定，须比较真实区间条件：开上界档要求 r < hi。
    """
    # 第一轮：严格区间命中（lo ≤ r 且 r < hi 开上界 / r ≤ hi 闭上界）
    strict = []
    for lo, hi, bid, name in bands:
        if (lo is None or r >= lo) and r < hi:
            strict.append((lo, hi, bid, name))
        elif (lo is None or r >= lo) and r == hi:
            # r 恰等于闭上界档（between/lte）的 hi → 该档命中（如 BALANCED [0.9,1.1] 命中 R=1.1）
            # 与开上界档（lt）区分：开上界在 r==hi 时不命中
            strict.append((lo, hi, bid, name)) if _has_closed_upper(
                bid, bands
            ) else None
    if len(strict) == 1:
        return strict[0][2], strict[0][3]
    if len(strict) > 1:
        # 多重命中取最紧上界档（区间重叠时的确定性规则；正常规则集不触发）
        strict.sort(key=lambda b: b[1])
        return strict[0][2], strict[0][3]
    # 无严格命中（r 落在档界间隙或超出覆盖）→ 兜底最紧上界
    for lo, hi, bid, name in bands:
        if r <= hi:
            return bid, name
    return "UNRESOLVED", "超出规则档位覆盖范围（检查 L3 规则包区间连续性）"


def _has_closed_upper(bid: str, bands: list[tuple]) -> bool:
    """该档上界是否为闭（between/lte）。当前规则集：STRONG_COMM（lt）与
    COMM_BOUND（lt 上界）为开，BALANCED/COMPUTE_BOUND 为闭。按 id 约定
    强通信/通信档上界开、其余闭——由 r_bands_from_rules 的区间形状保证。"""
    return bid not in ("L3_MC2_STRONG_COMM", "L3_MC2_COMM_BOUND")


def _collect_metric_leaves(cond, metric: str) -> list[dict]:
    """递归收集条件树中引用指定 metric 的叶子。"""
    out = []
    if cond is None:
        return out
    if isinstance(cond, list):
        for sub in cond:
            out.extend(_collect_metric_leaves(sub, metric))
        return out
    if isinstance(cond, dict):
        if "metric" in cond and cond["metric"] == metric:
            out.append(cond)
        for key in ("all", "any"):
            if key in cond:
                out.extend(_collect_metric_leaves(cond[key], metric))
    return out


def parse_task_durations(prof_dir: str) -> dict[str, list[float]]:
    """从 msprof 产出的 op_summary_*.csv 提取 Task Duration(us)，按文件分组返回。

    每个文件 ≈ 一个 rank 的独立 op_summary（msprof 每设备一份 PROF 目录）——
    分组是「逐 rank 稳态均值再跨 rank 取 max」（木桶口径）的前提（检视 F19：
    全部行统一求均值在数学上不等价，mean(所有行) ≤ max(各 rank 均值)，
    系统性低估 T → R 偏向「计算 bound」结论）。
    返回 {文件名: [时长列表]}；空文件不产生键。
    """
    groups: dict[str, list[float]] = {}
    for p in sorted(Path(prof_dir).rglob("op_summary_*.csv")):
        vals = []
        with open(p, encoding="utf-8-sig", errors="replace") as f:
            for row in csv.DictReader(f):
                v = row.get("Task Duration(us)") or row.get("Task Duration")
                if v:
                    try:
                        vals.append(float(str(v).strip()))
                    except ValueError:
                        pass
        if vals:
            groups[str(p)] = vals
    return groups


def steady_max(grouped: dict[str, list[float]], warmup: int) -> float | None:
    """木桶口径稳态聚合（检视 F19）：每 rank（文件组）内丢弃前 warmup 次取均值，
    再跨 rank 取 max——所有 rank 经 BarrierAll/CrossCoreFlag 同步，整体性能由
    最慢 rank 决定。"""
    per_rank = []
    for vals in grouped.values():
        steady = vals[warmup:] if len(vals) > warmup else vals
        if steady:
            per_rank.append(sum(steady) / len(steady))
    return max(per_rank) if per_rank else None


def parse_pipe_ratio(prof_dir: str, columns: list[str]) -> dict[str, float]:
    """从 op_summary_*.csv 提取 pipe 占比列的逐 rank 稳态均值（F8：mte2_ratio_pure 等的生产者）。

    返回 {文件名: 稳态均值}；列不存在或全空 → 空 dict。
    """
    groups: dict[str, float] = {}
    for p in sorted(Path(prof_dir).rglob("op_summary_*.csv")):
        vals = []
        with open(p, encoding="utf-8-sig", errors="replace") as f:
            for row in csv.DictReader(f):
                for col in columns:
                    v = row.get(col)
                    if v and str(v).strip() not in ("", "N/A", "NA", "-"):
                        try:
                            fv = float(str(v).strip().replace("%", ""))
                            vals.append(
                                fv * 100 if 0 <= fv <= 1.5 else fv
                            )  # ratio_to_pct 兼容
                            break
                        except ValueError:
                            pass
        if vals:
            groups[str(p)] = sum(vals) / len(vals)
    return groups


def parse_switch(spec: str) -> dict:
    """把 --comm-switch/--compute-switch 解析成环境变量注入 dict。

    支持两种形式：
      - 纯 KEY=VALUE 或 KEY=VALUE,K2=V2：注入这些变量（默认 MC2_MODE=COMM_ONLY 等）
      - 裸字符串（无 =）：视为提示文本，不注入（打印用）
    """
    env = {}
    for kv in spec.split(","):
        kv = kv.strip()
        if "=" in kv:
            k, v = kv.split("=", 1)
            env[k.strip()] = v.strip()
    return env


def run_msprof_task_based(
    exe: str, exe_args: str, out: str, env_extra: dict
) -> dict[str, list[float]]:
    """msprof task-based 采集一轮（MC² 专用路径），返回按 op_summary 文件（≈按 rank）
    分组的 Task Duration。

    env 采用整体替换语义（subprocess.run(env=...) 不继承父进程环境），因此必须
    以 os.environ 为底、env_extra 覆盖——否则 PATH/ASCEND_HOME_PATH/LD_LIBRARY_PATH
    全部丢失，msprof 无法运行（检视 F2）。
    """
    env = {**os.environ, **env_extra}
    cmd = [
        "msprof",
        f"--output={out}",
        "--ai-core=on",
        "--aic-mode=task-based",
        "--aic-metrics=PipeUtilization",
        "--task-time=on",
        "--ascendcl=on",
        f"--application={exe} {exe_args}",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=1800, env=env)
    if proc.returncode != 0:
        raise RuntimeError(f"msprof 失败: {proc.stderr[:400]}")
    return parse_task_durations(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="MC² 隔离测试编排（T_comm/T_compute → R）")
    ap.add_argument(
        "--exe", required=True, help="待测可执行文件（须按 rank 数自行 fork）"
    )
    ap.add_argument(
        "--args", default="", help='程序参数（如 "M K N rankNum perf tileCnt"）'
    )
    ap.add_argument(
        "--comm-switch",
        default="MC2_MODE=COMM_ONLY",
        help="纯通信开关（K=V 或 K1=V1,K2=V2 环境变量形式，注入采集子进程；"
        "源码注释模式由 Agent 在副本完成后调用本脚本 --from-prof 收敛）",
    )
    ap.add_argument(
        "--compute-switch",
        default="MC2_MODE=COMPUTE_ONLY",
        help="纯计算开关（同上形式，注入采集子进程）",
    )
    ap.add_argument(
        "--switch-mode",
        default="env",
        choices=["env", "from-prof"],
        help="env=程序自带模式开关；from-prof=已手工注释的两份 profiling 目录直接给路径",
    )
    ap.add_argument(
        "--comm-prof", default=None, help="from-prof 模式：纯通信 profiling 目录"
    )
    ap.add_argument(
        "--compute-prof", default=None, help="from-prof 模式：纯计算 profiling 目录"
    )
    ap.add_argument("--prof-dir", default="./prof/iso", help="env 模式采集落盘根目录")
    ap.add_argument(
        "--rules",
        default=str(DEFAULT_L3_RULES),
        help="L3 规则包 YAML（R 值档位唯一真源；默认仓内 L3-inter-card.yaml）",
    )
    ap.add_argument("--loops", type=int, default=LOOPS)
    ap.add_argument("--warmup", type=int, default=WARMUP)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    if args.switch_mode == "from-prof":
        comm_groups = parse_task_durations(args.comm_prof)
        compute_groups = parse_task_durations(args.compute_prof)
        if not comm_groups or not compute_groups:
            print(
                "ERROR: from-prof 目录中未找到 op_summary Task Duration 数据",
                file=sys.stderr,
            )
            return 1
        t_comm, t_compute = (
            steady_max(comm_groups, args.warmup),
            steady_max(compute_groups, args.warmup),
        )
        t_full, full_groups, mte2_ratio_full = None, {}, None
        detail = {
            "mode": "from-prof",
            "comm_prof": args.comm_prof,
            "compute_prof": args.compute_prof,
        }
        # F8：纯计算路径 pipe 占比（mte2_ratio_pure 的生产者——隔离计算 run 的
        # aic_mte2_ratio 稳态均值，跨 rank 取 max，与 T 口径一致）
        compute_pure_ratio = parse_pipe_ratio(args.compute_prof, MTE2_RATIO_COLUMNS)
        mte2_ratio_pure = (
            round(max(compute_pure_ratio.values()), 1) if compute_pure_ratio else None
        )
    else:
        # env 开关模式：--comm-switch/--compute-switch 支持 K=V（或 K1=V1,K2=V2）形式，
        # 解析出的变量真正注入子进程 env（消灭死参数，检视 F2/N2）
        comm_env = parse_switch(args.comm_switch) or {"MC2_MODE": "COMM_ONLY"}
        compute_env = parse_switch(args.compute_switch) or {"MC2_MODE": "COMPUTE_ONLY"}
        comm_out = str(Path(args.prof_dir) / "comm")
        compute_out = str(Path(args.prof_dir) / "compute")
        print(f"[iso] 采集纯通信（{args.comm_switch}）…")
        comm_groups = run_msprof_task_based(args.exe, args.args, comm_out, comm_env)
        print(f"[iso] 采集纯计算（{args.compute_switch}）…")
        compute_groups = run_msprof_task_based(
            args.exe, args.args, compute_out, compute_env
        )
        print("[iso] 采集融合态（全量，F8：mte2_ratio_full / mask_ratio 生产者）…")
        full_out = str(Path(args.prof_dir) / "full")
        full_groups = run_msprof_task_based(args.exe, args.args, full_out, {})
        t_comm, t_compute = (
            steady_max(comm_groups, args.warmup),
            steady_max(compute_groups, args.warmup),
        )
        t_full = steady_max(full_groups, args.warmup)
        detail = {
            "mode": "env",
            "comm_ranks": len(comm_groups),
            "compute_ranks": len(compute_groups),
            "full_ranks": len(full_groups),
        }
        compute_pure_ratio = parse_pipe_ratio(compute_out, MTE2_RATIO_COLUMNS)
        mte2_ratio_pure = (
            round(max(compute_pure_ratio.values()), 1) if compute_pure_ratio else None
        )
        full_ratio = parse_pipe_ratio(full_out, MTE2_RATIO_COLUMNS)
        mte2_ratio_full = round(max(full_ratio.values()), 1) if full_ratio else None

    if not t_comm or not t_compute:
        print("ERROR: 稳态数据为空（检查 op_summary 行数 ≥ loops）", file=sys.stderr)
        return 1

    # F8：rank_imbalance = max(rank 稳态均值)/mean(rank 稳态均值)（融合态，跨 rank 木桶离散度）
    rank_imbalance = None
    if t_full is not None and full_groups:
        rank_means = []
        for vals in full_groups.values():
            steady = vals[args.warmup :] if len(vals) > args.warmup else vals
            if steady:
                rank_means.append(sum(steady) / len(steady))
        if len(rank_means) >= 2 and sum(rank_means) > 0:
            rank_imbalance = round(
                max(rank_means) / (sum(rank_means) / len(rank_means)), 3
            )

    # F8：mask_ratio = (融合前耗时 − 融合后耗时) / min(T_comm, T_compute)，上限 1.0
    # 融合前耗时 = T_comm + T_compute（隔离态串行和）；融合后 = t_full
    mask_ratio = None
    if t_full is not None and t_comm and t_compute:
        denom = min(t_comm, t_compute)
        if denom > 0:
            mask_ratio = round(min((t_comm + t_compute - t_full) / denom, 1.0), 3)

    r = t_compute / t_comm
    # 档位从 L3 规则包 YAML 派生（规则是唯一真源；解析失败在 r_bands_from_rules 里 fail loud）
    bound, bound_name = classify_r(r, r_bands_from_rules(args.rules))

    # F8：产出直接拼成 verdict.py 的 --scalar 串（MC² 旗舰判定链端到端可执行）
    scalar_parts = [f"t_comm_us={t_comm:.0f}", f"t_compute_us={t_compute:.0f}"]
    if mte2_ratio_full is not None:
        scalar_parts.append(f"mte2_ratio_full={mte2_ratio_full:.0f}")
    if mte2_ratio_pure is not None:
        scalar_parts.append(f"mte2_ratio_pure={mte2_ratio_pure:.0f}")
    if rank_imbalance is not None:
        scalar_parts.append(f"rank_imbalance={rank_imbalance}")
    if mask_ratio is not None:
        scalar_parts.append(f"mask_ratio={mask_ratio}")
    scalar_str = ",".join(scalar_parts)

    result = {
        "schema": "opstune.isolation/1.0",
        "t_comm_us": round(t_comm, 2),
        "t_compute_us": round(t_compute, 2),
        "t_full_us": round(t_full, 2) if t_full is not None else None,
        "r_value": round(r, 3),
        "bound_hint": bound,
        "bound_name": bound_name,
        "mte2_ratio_pure": mte2_ratio_pure,
        "mte2_ratio_full": mte2_ratio_full,
        "rank_imbalance": rank_imbalance,
        "mask_ratio": mask_ratio,
        "verdict_scalar": scalar_str,
        "r_bands_source": str(Path(args.rules).name),
        "steady_rule": (
            f"{args.loops} 轮前 {args.warmup} warm-up 后逐 rank 均值，"
            "跨 rank 取 max（木桶效应）"
        ),
        "detail": detail,
        "next": (
            f"喂判定引擎：python3 ../ops-verdict/scripts/verdict.py "
            f"--rules ../ops-verdict/rules/L3-inter-card.yaml,../ops-verdict/rules/L1-single-core.yaml "
            f"--soc a5 --scalar {scalar_str} -o verdict_mc2.json"
        ),
    }
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(
        json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(f"T_comm={t_comm:.1f}us  T_compute={t_compute:.1f}us  R={r:.3f} → {bound}")
    if mte2_ratio_pure is not None or mte2_ratio_full is not None:
        print(f"mte2_ratio: pure={mte2_ratio_pure} full={mte2_ratio_full}")
    if rank_imbalance is not None:
        print(f"rank_imbalance={rank_imbalance}  mask_ratio={mask_ratio}")
    print(f"→ {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
