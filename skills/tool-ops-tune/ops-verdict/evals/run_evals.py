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
"""run_evals.py — 判定引擎执行式回归（fixture CSV/标量 → 断言 verdict 输出）

设计立场：判定知识若不进脚本，就无法回归——文本形式的评测对判定质量
  没有守护力。判定已经进脚本（verdict.py），评测必须跟着进脚本：
  任何阈值/规则变更必须先过本回归全绿。

用例分两组（共 38 条）：
  - E1-E26：verdict.py 判定回归（fixture → 断言 findings/verdict；含 R 值
    临界值双侧守护 E12b/E12c——区间重叠只在边界值暴露）
  - S1-S12：门禁脚本回归（snapshot/delta_compare/batch/加载校验/边界）——
    Step 4 的结论门禁（KEEP/REVERT 判错 = 错误归档）与加载校验负例（F10b 扩容，
    检视 F10/F3/F4/F6/F7/F18 的回归网）

用法：
  python3 evals/run_evals.py            # 全部用例
  python3 evals/run_evals.py -v         # 逐断言明细
退出码：0 全绿 / 1 有失败（CI 门禁语义）。
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SKILL_DIR = HERE.parent
RULES = SKILL_DIR / "rules"
FIXTURES = SKILL_DIR / "fixtures"
SCRIPTS = SKILL_DIR / "scripts"
VERDICT = SCRIPTS / "verdict.py"
SNAPSHOT = SCRIPTS / "snapshot.py"
DELTA_COMPARE = SCRIPTS / "delta_compare.py"

# ---------------------------------------------------------------------------
# 用例定义：输入 → 断言（findings / skipped / verdict / headline 片段）
# 每条用例对应一个真实调优场景的已知答案（源自真机调优案例标定）。
# ---------------------------------------------------------------------------
CASES = [
    {
        "id": "E1-mte2-bound",
        "desc": "MTE2 占比 78% 且为最大 pipe → MTE2_BOUND 命中（主判定阈值表）",
        "args": [
            "--rules",
            str(RULES / "L1-single-core.yaml"),
            "--csv",
            str(FIXTURES / "mte2_bound_a2.csv"),
            "--soc",
            "a2",
        ],
        "assert": {
            "hit": ["L1_MTE2_BOUND"],
            "not_hit": ["L1_VEC_BOUND", "L1_CUBE_BOUND", "L1_LATENCY_SYNC_SUSPECT"],
            "verdict": "NEEDS_OPTIMIZATION",
        },
    },
    {
        "id": "E2-mte2-counter-evidence",
        "desc": "同 E1 输入 + active_bw=85%峰值 → 命中反证『真带宽饱和』（优化方向改变）",
        "args": [
            "--rules",
            str(RULES / "L1-single-core.yaml"),
            "--csv",
            str(FIXTURES / "mte2_bound_a2.csv"),
            "--soc",
            "a2",
            "--scalar",
            "mte2_active_bw_pct_of_peak=85",
        ],
        "assert": {
            "hit": ["L1_MTE2_BOUND"],
            "counter_evidence": ["true-bandwidth-saturation"],
        },
    },
    {
        "id": "E3-serial-no-overlap",
        "desc": "cube 0.44+mte2 0.42+mte1 0.18≈1 且最大 pipe<70% → 串行无重叠指纹命中（串行无重叠实测案例）",
        "args": [
            "--rules",
            str(RULES / "L1-single-core.yaml"),
            "--csv",
            str(FIXTURES / "serial_no_overlap_a2.csv"),
            "--soc",
            "a2",
        ],
        "assert": {
            "hit": ["L1_SERIAL_NO_OVERLAP"],
            "not_hit": ["L1_CUBE_BOUND", "L1_MTE2_BOUND"],
        },
    },
    {
        "id": "E4-core-tail",
        "desc": "逐核耗时均值~93us 峰值~185us（≈2x）→ 核间长尾命中 critical（A5 48 核场景）",
        "args": [
            "--rules",
            str(RULES / "L2-inter-core.yaml"),
            "--csv",
            str(FIXTURES / "core_tail_a5.csv"),
            "--soc",
            "a5",
            "--scalar",
            "physical_cores=48,block_dim=24,task_duration_us=195",
        ],
        "assert": {
            "hit": ["L2_CORE_TAIL", "L2_IMBALANCE_SEVERE"],
            "not_hit": ["L2_UNDERUTILIZATION"],
        },
    },
    {
        "id": "E5-underutilization",
        "desc": "仅 8 个 block 行（<48×0.33）→ 切核不足命中（occupancy 判定档）",
        "args": [
            "--rules",
            str(RULES / "L2-inter-core.yaml"),
            "--csv",
            str(FIXTURES / "serial_no_overlap_a2.csv"),
            "--soc",
            "a5",
            "--scalar",
            "physical_cores=48,task_duration_us=620",
        ],
        "assert": {"hit": ["L2_UNDERUTILIZATION"]},
    },
    {
        "id": "E6-mc2-compute-bound",
        "desc": "T_comm=420 T_compute=880 → R=2.10 → 强计算 bound（R 值五档判定）",
        "args": [
            "--rules",
            str(RULES / "L3-inter-card.yaml"),
            "--soc",
            "a5",
            "--scalar",
            "t_comm_us=420,t_compute_us=880",
        ],
        "assert": {
            "hit": ["L3_MC2_STRONG_COMPUTE"],
            "not_hit": ["L3_MC2_BALANCED", "L3_MC2_COMM_BOUND"],
        },
    },
    {
        "id": "E7-mc2-balanced",
        "desc": "T_comm=500 T_compute=505 → R=1.01 → 完美平衡（等分切分即最优）",
        "args": [
            "--rules",
            str(RULES / "L3-inter-card.yaml"),
            "--soc",
            "a5",
            "--scalar",
            "t_comm_us=500,t_compute_us=505",
        ],
        "assert": {
            "hit": ["L3_MC2_BALANCED"],
            "not_hit": ["L3_MC2_COMPUTE_BOUND", "L3_MC2_COMM_BOUND"],
        },
    },
    {
        "id": "E8-mc2-mte2-pollution-routing",
        "desc": "ratio_full=62 ratio_pure=41（gap=21>10 且 pure/full=66%<80%）→ 污染命中 + L1_MTE2_BOUND 被禁用",
        "args": [
            "--rules",
            str(RULES / "L3-inter-card.yaml")
            + ","
            + str(RULES / "L1-single-core.yaml"),
            "--soc",
            "a5",
            "--scalar",
            "t_comm_us=400,t_compute_us=600,mte2_ratio_full=62,mte2_ratio_pure=41",
        ],
        "assert": {
            "hit": ["L3_MTE2_POLLUTED"],
            "hit_either": ["L3_MC2_COMPUTE_BOUND"],
            "skipped_contains": ["L1_MTE2_BOUND"],
            "skipped_reason_contains": {"L1_MTE2_BOUND": "被上层路由禁用"},
        },
    },
    {
        "id": "E9-binding-contract-skip",
        "desc": "L1 规则包但无 CSV 且无标量 → REQUIRED 指标缺失 → SKIPPED（绑定契约：未采集不判定）",
        "args": ["--rules", str(RULES / "L1-single-core.yaml"), "--soc", "a5"],
        "assert": {
            "skipped_contains": ["L1_MTE2_BOUND"],
            "verdict": "INSUFFICIENT_DATA",
        },
    },
    {
        "id": "E10-no-bound-near-limit",
        "desc": "各 pipe 均 <50% 且求和不落串行指纹 → LATENCY/SYNC 可疑命中而非『无 bound』",
        "args": [
            "--rules",
            str(RULES / "L1-single-core.yaml"),
            "--csv",
            str(FIXTURES / "serial_no_overlap_a2.csv"),
            "--soc",
            "a2",
        ],
        "assert": {"hit_or_insufficient": ["L1_LATENCY_SYNC_SUSPECT"]},
    },
    {
        "id": "E11-r-boundary-089-vs-090",
        "desc": "F3 边界：R=0.89 → COMM_BOUND（唯一）；R=0.90 → BALANCED（唯一）——重叠区已消除",
        "args": [
            "--rules",
            str(RULES / "L3-inter-card.yaml"),
            "--soc",
            "a5",
            "--scalar",
            "t_comm_us=1000,t_compute_us=890",
        ],
        "assert": {"hit": ["L3_MC2_COMM_BOUND"], "not_hit": ["L3_MC2_BALANCED"]},
    },
    {
        "id": "E12-r-boundary-0110-vs-0111",
        "desc": "F3 边界：R=1.10 → BALANCED；R=1.11 → COMPUTE_BOUND（档位连续无重叠无双命中）",
        "args": [
            "--rules",
            str(RULES / "L3-inter-card.yaml"),
            "--soc",
            "a5",
            "--scalar",
            "t_comm_us=1000,t_compute_us=1110",
        ],
        "assert": {"hit": ["L3_MC2_COMPUTE_BOUND"], "not_hit": ["L3_MC2_BALANCED"]},
    },
    {
        "id": "E12b-r-boundary-090-balanced",
        "desc": "F3 临界值（检视补）：R=0.90 → 仅 BALANCED 命中——COMM_BOUND 上界与 BALANCED 下界在 0.90 不重叠（改 <= 即双命中，本用例守护）",
        "args": [
            "--rules",
            str(RULES / "L3-inter-card.yaml"),
            "--soc",
            "a5",
            "--scalar",
            "t_comm_us=1000,t_compute_us=900",
        ],
        "assert": {
            "hit": ["L3_MC2_BALANCED"],
            "not_hit": ["L3_MC2_COMM_BOUND", "L3_MC2_COMPUTE_BOUND"],
        },
    },
    {
        "id": "E12c-r-boundary-110-balanced",
        "desc": "F3 临界值（检视补）：R=1.10 → 仅 BALANCED 命中——BALANCED 上界与 COMPUTE_BOUND 下界在 1.10 不重叠（改 <= 即双命中，本用例守护）",
        "args": [
            "--rules",
            str(RULES / "L3-inter-card.yaml"),
            "--soc",
            "a5",
            "--scalar",
            "t_comm_us=1000,t_compute_us=1100",
        ],
        "assert": {
            "hit": ["L3_MC2_BALANCED"],
            "not_hit": ["L3_MC2_COMPUTE_BOUND", "L3_MC2_COMM_BOUND"],
        },
    },
    {
        "id": "E13-waterfall-layered-facts",
        "desc": "F4 全瀑布：facts 按包分层，L1 pipe 指标不丢失（审计承诺）",
        "args": [
            "--rules",
            str(RULES / "L3-inter-card.yaml")
            + ","
            + str(RULES / "L2-inter-core.yaml")
            + ","
            + str(RULES / "L1-single-core.yaml")
            + ","
            + str(RULES / "L0-instruction.yaml"),
            "--csv",
            str(FIXTURES / "mte2_bound_a2.csv"),
            "--soc",
            "a5",
            "--scalar",
            "t_comm_us=400,t_compute_us=600,icache_miss_rate=20,l2_hit_rate=40,physical_cores=48,block_dim=24,task_duration_us=620",
        ],
        "assert": {
            "facts_pack_contains": {
                "L1-single-core": ["vec_ratio", "mte2_ratio"],
                "L2-inter-core": ["time_mean_us"],
                "L3-inter-card": ["r_value"],
                "L0-instruction": ["icache_miss_rate"],
            }
        },
    },
    {
        "id": "E14-l2-head-overhead-derived",
        "desc": "F6 头开销：task_duration=700（头开销 31%>30%）→ L2_HEAD_OVERHEAD 命中（修复前永 SKIPPED）",
        "args": [
            "--rules",
            str(RULES / "L2-inter-core.yaml"),
            "--soc",
            "a5",
            "--csv",
            str(FIXTURES / "mte2_bound_a2.csv"),
            "--scalar",
            "physical_cores=48,task_duration_us=700",
        ],
        "assert": {"hit": ["L2_HEAD_OVERHEAD"]},
    },
    {
        "id": "E15-occupancy-critical-band",
        "desc": "F6 分档：4 活跃核/48 物理核（8.3%<10%）→ L2_UNDERUTILIZATION critical 档生效（修复前永 light）",
        "args": [
            "--rules",
            str(RULES / "L2-inter-core.yaml"),
            "--soc",
            "a5",
            "--csv",
            str(FIXTURES / "few_cores_a5.csv"),
            "--scalar",
            "physical_cores=48,task_duration_us=120",
        ],
        "assert": {
            "hit": ["L2_UNDERUTILIZATION"],
            "severity": {"L2_UNDERUTILIZATION": "critical"},
        },
    },
    {
        "id": "E16-near-limit-branch",
        "desc": "F7 接近上限：唯一 critical+high 命中且上界 1.2 ≤ 阈值 1.2 → NEAR_LIMIT（修复前死分支）",
        "args": [
            "--rules",
            str(RULES / "L3-inter-card.yaml"),
            "--soc",
            "a5",
            "--scalar",
            "t_comm_us=400,t_compute_us=1000",
        ],
        "assert": {"verdict": "NEAR_LIMIT", "hit": ["L3_MC2_STRONG_COMPUTE"]},
    },
    {
        "id": "E17-no-bound-found-branch",
        "desc": "F7 语义分离：数据完整但无命中 → NO_BOUND_FOUND（≠ NEAR_LIMIT；下游可区分收手/换指标）",
        "args": [
            "--rules",
            str(RULES / "L1-single-core.yaml"),
            "--soc",
            "a5",
            "--csv",
            str(FIXTURES / "no_bound_a5.csv"),
        ],
        "assert": {"verdict": "NO_BOUND_FOUND"},
    },
    {
        "id": "E18-dirty-scalar-degrade",
        "desc": "F18 脏输入：--scalar 值 N/A → 警告 + None 降级，引擎不崩溃，反证标 unevaluated",
        "args": [
            "--rules",
            str(RULES / "L1-single-core.yaml"),
            "--soc",
            "a2",
            "--csv",
            str(FIXTURES / "mte2_bound_a2.csv"),
            "--scalar",
            "mte2_active_bw_pct_of_peak=N/A",
        ],
        "assert": {
            "hit": ["L1_MTE2_BOUND"],
            "counter_evidence_unevaluated": {
                "L1_MTE2_BOUND": ["true-bandwidth-saturation"]
            },
        },
    },
    {
        "id": "E19-vec-counter-evidence-scalar",
        "desc": "F5 反证复活：--scalar vec_fp32_ratio=61 → cast-heavy 反证命中（修复前永久 unevaluated）",
        "args": [
            "--rules",
            str(RULES / "L1-single-core.yaml"),
            "--soc",
            "a5",
            "--csv",
            str(FIXTURES / "vec_bound_a5.csv"),
            "--scalar",
            "vec_fp32_ratio=61,vec_total_cflt_ratio=8",
        ],
        "assert": {
            "hit": ["L1_VEC_BOUND"],
            "counter_evidence_hit": {
                "L1_VEC_BOUND": ["cast-heavy", "bank-conflict-amplify"]
            },
        },
    },
    {
        "id": "E20-multikernel-no-name-fail-loud",
        "desc": "G15：多 kernel CSV 无 --kernel-name → exit 4 拒判（修复前静默跨 kernel 平均错判）",
        "args": [
            "--rules",
            str(RULES / "L1-single-core.yaml"),
            "--csv",
            str(FIXTURES / "multi_kernel_mix_a5.csv"),
            "--soc",
            "a5",
        ],
        "assert": {"exit_code": 4},
    },
    {
        "id": "E21-multikernel-filtered",
        "desc": "G15：--kernel-name matmul_mix 过滤后单 kernel 判定（cube 87.5% critical 命中，"
        "混合均值会被 gelu 行拉低到 ~50 错过 critical 档）",
        "args": [
            "--rules",
            str(RULES / "L1-single-core.yaml"),
            "--csv",
            str(FIXTURES / "multi_kernel_mix_a5.csv"),
            "--soc",
            "a5",
            "--kernel-name",
            "matmul_mix",
        ],
        "assert": {
            "hit": ["L1_CUBE_BOUND"],
            "severity": {"L1_CUBE_BOUND": "critical"},
            "rows": 4,
        },
    },
    {
        "id": "E22-multikernel-bad-name-fail-loud",
        "desc": "G15：--kernel-name 拼错 → exit 4（不静默空集判定）",
        "args": [
            "--rules",
            str(RULES / "L1-single-core.yaml"),
            "--csv",
            str(FIXTURES / "multi_kernel_mix_a5.csv"),
            "--soc",
            "a5",
            "--kernel-name",
            "no_such_kernel",
        ],
        "assert": {"exit_code": 4},
    },
    {
        "id": "E23-host-dispatch-cluster-rule",
        "desc": "P2-4/G17：host_dispatch_us=80 / task=200（40%>30%）→ L2_HOST_DISPATCH_BLOCK 命中 moderate",
        "args": [
            "--rules",
            str(RULES / "L2-inter-core.yaml"),
            "--soc",
            "a5",
            "--csv",
            str(FIXTURES / "serial_no_overlap_a2.csv"),
            "--scalar",
            "host_dispatch_us=80,task_duration_us=200",
        ],
        "assert": {
            "hit": ["L2_HOST_DISPATCH_BLOCK"],
            "severity": {"L2_HOST_DISPATCH_BLOCK": "moderate"},
        },
    },
    {
        "id": "E24-host-dispatch-no-scalar-skip",
        "desc": "P2-4/G17：无 host_dispatch_us 标量 → SKIPPED（绑定契约：无生产者不判定）",
        "args": [
            "--rules",
            str(RULES / "L2-inter-core.yaml"),
            "--soc",
            "a5",
            "--csv",
            str(FIXTURES / "serial_no_overlap_a2.csv"),
        ],
        "assert": {"skipped_contains": ["L2_HOST_DISPATCH_BLOCK"]},
    },
]
# ---------------------------------------------------------------------------
# 门禁脚本回归（F10b：snapshot/delta_compare 的 exit 码与三段结论边界 +
# 加载校验负例）——Step 4 结论门禁的回归网
# ---------------------------------------------------------------------------
SCRIPT_CASES = [
    {"id": "S1-delta-hard-reject", "desc": "口径漂移（device 0→1）→ exit 2 拒绝比较"},
    {"id": "S2-delta-precision-reject", "desc": "精度非 pass → exit 2 拒绝比较"},
    {"id": "S3-delta-keep", "desc": "speedup≈1.053（>1.05 边界）→ KEEP"},
    {
        "id": "S4-delta-noise-band",
        "desc": "speedup≈0.992（带内边界）→ 复测处置（含同步重采基线文案）",
    },
    {"id": "S5-delta-revert", "desc": "speedup≈0.909 → REVERT"},
    {"id": "S6-mechanism-warn", "desc": "tiling_key/block_dim 变化 → WARN 记录不阻断"},
    {"id": "S7-repeat-conflict", "desc": "repeat 20 vs 40 → exit 2（HARD 不变量）"},
    {"id": "S8-noise-band-override", "desc": "--noise-band 0.01 覆写 → 1% 差异判劣化"},
    {
        "id": "S9-jitter-diagnostics",
        "desc": "分布字段（p95>2×p50）→ 抖动诊断段 + 双峰提示",
    },
    {"id": "S10-validation-v1", "desc": "加载校验 V1：未声明指标 → exit 3"},
    {"id": "S11-validation-v3", "desc": "加载校验 V3：区间重叠（F3 形态）→ exit 3"},
    {"id": "S12-validation-v4", "desc": "加载校验 V4：optional 进 condition → exit 3"},
]


def _mk_snap(td, name, **over):
    """生成一份 snapshot 并返回路径。"""
    fields = {
        "op": "gelu",
        "variant": name,
        "soc": "a5",
        "precision": "pass",
        "kernel_avg_us": 100.0,
        "timing_method": "event",
        "baseline_kind": "self_before_after",
        "cann_version": "8.3.0",
        "device_id": "0",
        "shape": "8448x4096",
        "dtype": "fp16",
        "format": "ND",
        "warmup": 5,
        "repeat": 20,
    }
    fields.update(over)
    cmd = [sys.executable, str(SNAPSHOT), "--no-env-probe"]
    for k, v in fields.items():
        flag = "--" + ("kernel-avg-us" if k == "kernel_avg_us" else k.replace("_", "-"))
        cmd += [flag, str(v)]
    cmd += ["-o", str(Path(td) / f"{name}.json")]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    assert proc.returncode == 0, proc.stderr
    return str(Path(td) / f"{name}.json")


def _run_delta(td, base, after, extra=None):
    return subprocess.run(
        [
            sys.executable,
            str(DELTA_COMPARE),
            "--baseline",
            base,
            "--after",
            after,
            "--output",
            str(Path(td) / "comp.md"),
            *(extra or []),
        ],
        capture_output=True,
        text=True,
        timeout=60,
    )


def _run_with_pack(td, pack):
    import yaml as _yaml

    rp = Path(td) / "test.yaml"
    rp.write_text(_yaml.dump(pack, allow_unicode=True))
    p = subprocess.run(
        [
            sys.executable,
            str(VERDICT),
            "--rules",
            str(rp),
            "--soc",
            "a5",
            "-o",
            str(Path(td) / "v.json"),
        ],
        capture_output=True,
        text=True,
        timeout=60,
    )
    return p.returncode, p.stderr


def run_script_case(case: dict, verbose: bool) -> tuple[bool, list[str]]:
    errs = []
    with tempfile.TemporaryDirectory() as td:
        cid = case["id"]
        if cid == "S1-delta-hard-reject":
            b = _mk_snap(td, "baseline")
            a = _mk_snap(td, "after", device_id="1")
            p = _run_delta(td, b, a)
            if p.returncode != 2 or "device_id" not in p.stderr:
                errs.append(f"应 exit 2 且 stderr 含 device_id（rc={p.returncode}）")
        elif cid == "S2-delta-precision-reject":
            b = _mk_snap(td, "baseline")
            a = _mk_snap(td, "after", precision="fail")
            p = _run_delta(td, b, a)
            if p.returncode != 2 or "精度" not in p.stderr:
                errs.append(f"应 exit 2 且 stderr 含精度（rc={p.returncode}）")
        elif cid == "S3-delta-keep":
            b = _mk_snap(td, "baseline", kernel_avg_us=100.0)
            a = _mk_snap(td, "after", kernel_avg_us=95.0)  # 1.0526 > 1.05
            p = _run_delta(td, b, a)
            if p.returncode != 0 or "KEEP" not in p.stdout:
                errs.append(f"应 KEEP（rc={p.returncode}, out={p.stdout[:80]}）")
        elif cid == "S4-delta-noise-band":
            b = _mk_snap(td, "baseline", kernel_avg_us=100.0)
            a = _mk_snap(td, "after", kernel_avg_us=100.8)  # 0.992 within band
            p = _run_delta(td, b, a)
            if (
                p.returncode != 0
                or "噪声带内" not in p.stdout
                or "同步重采基线" not in p.stdout
            ):
                errs.append(f"应噪声带内 + 同步重采文案（out={p.stdout[:120]}）")
        elif cid == "S5-delta-revert":
            b = _mk_snap(td, "baseline", kernel_avg_us=100.0)
            a = _mk_snap(td, "after", kernel_avg_us=110.0)  # 0.909
            p = _run_delta(td, b, a)
            if p.returncode != 0 or "REVERT" not in p.stdout:
                errs.append(f"应 REVERT（out={p.stdout[:80]}）")
        elif cid == "S6-mechanism-warn":
            b = _mk_snap(td, "baseline", tiling_key="tiling_v1", block_dim=24)
            a = _mk_snap(
                td, "after", tiling_key="tiling_v2", block_dim=48, kernel_avg_us=95.0
            )
            p = _run_delta(td, b, a)
            if p.returncode != 0 or "WARN" not in p.stdout:
                errs.append(f"机制变量应 WARN 不阻断（rc={p.returncode}）")
        elif cid == "S7-repeat-conflict":
            b = _mk_snap(td, "baseline", repeat=20)
            a = _mk_snap(td, "after", repeat=40)
            p = _run_delta(td, b, a)
            if p.returncode != 2 or "repeat" not in p.stderr:
                errs.append(f"repeat 漂移应 exit 2（rc={p.returncode}）")
        elif cid == "S8-noise-band-override":
            b = _mk_snap(td, "baseline", kernel_avg_us=100.0)
            a = _mk_snap(td, "after", kernel_avg_us=101.0)  # 0.990
            p = _run_delta(td, b, a, ["--noise-band", "0.01"])
            if p.returncode != 0 or "劣化" not in p.stdout:
                errs.append(f"noise-band 0.01 覆写后应判劣化（out={p.stdout[:80]}）")
        elif cid == "S9-jitter-diagnostics":
            b = _mk_snap(
                td,
                "baseline",
                kernel_p50_us=10.0,
                kernel_p95_us=35.0,
                kernel_max_us=40.0,
            )
            a = _mk_snap(
                td, "after", kernel_p50_us=12.0, kernel_p95_us=13.0, kernel_max_us=14.0
            )
            p = _run_delta(td, b, a)
            md = (Path(td) / "comp.md").read_text(encoding="utf-8")
            if "抖动诊断" not in md or "双峰" not in md:
                errs.append("应输出抖动诊断段 + 双峰提示")
        elif cid == "S10-validation-v1":
            pack = {
                "schema": "opstune.rules/1.0",
                "layer": "T",
                "metrics": {"foo": {"source": "scalar"}},
                "rules": [
                    {
                        "id": "T1",
                        "condition": {"metric": "undeclared", "op": "gt", "value": 1},
                        "metrics_required": ["undeclared"],
                    }
                ],
            }
            rc, err = _run_with_pack(td, pack)
            if rc != 3 or "[V1]" not in err:
                errs.append(f"V1 应 exit 3（rc={rc}）")
        elif cid == "S11-validation-v3":
            pack = {
                "schema": "opstune.rules/1.0",
                "layer": "T",
                "metrics": {"r": {"source": "derived"}},
                "rules": [
                    {
                        "id": "A",
                        "condition": {
                            "all": [
                                {"metric": "r", "op": "gte", "value": 0.5},
                                {"metric": "r", "op": "lt", "value": 1.0},
                            ]
                        },
                        "metrics_required": [],
                    },
                    {
                        "id": "B",
                        "condition": {
                            "all": [
                                {"metric": "r", "op": "between", "value": [0.9, 1.1]}
                            ]
                        },
                        "metrics_required": [],
                    },
                ],
            }
            rc, err = _run_with_pack(td, pack)
            if rc != 3 or "[V3]" not in err:
                errs.append(f"V3 应 exit 3（rc={rc}）")
        elif cid == "S12-validation-v4":
            pack = {
                "schema": "opstune.rules/1.0",
                "layer": "T",
                "metrics": {"foo": {"source": "scalar"}},
                "rules": [
                    {
                        "id": "T4",
                        "condition": {"metric": "foo", "op": "gt", "value": 1},
                        "metrics_required": [],
                        "metrics_optional": ["foo"],
                    }
                ],
            }
            rc, err = _run_with_pack(td, pack)
            if rc != 3 or "[V4]" not in err:
                errs.append(f"V4 应 exit 3（rc={rc}）")
    if verbose and errs:
        for e in errs:
            print(f"       ✗ {e}")
    return (not errs), errs


def run_case(case: dict, verbose: bool) -> tuple[bool, list[str]]:
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "verdict.json"
        cmd = [sys.executable, str(VERDICT), *case["args"], "-o", str(out)]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        if "exit_code" in case["assert"]:
            # 门禁类用例：只断言退出码与 stderr 片段（fail-loud 语义回归）
            if proc.returncode != case["assert"]["exit_code"]:
                return False, [
                    f"应 exit {case['assert']['exit_code']}，实际 {proc.returncode}"
                    f"（stderr: {proc.stderr[:200]}）"
                ]
            frag = case["assert"].get("stderr_contains")
            if frag and frag not in proc.stderr:
                return False, [f"stderr 应含 {frag!r}，实际 {proc.stderr[:200]!r}"]
            return True, []
        if proc.returncode != 0:
            return False, [f"verdict.py 退出码 {proc.returncode}: {proc.stderr[:300]}"]
        v = json.loads(out.read_text(encoding="utf-8"))

    errs = []
    a = case["assert"]
    hits = {f["rule_id"] for f in v["findings"] if f.get("status") == "HIT"}
    skipped = {s["rule_id"]: s for s in v["skipped"]}

    for rid in a.get("hit", []):
        if rid not in hits:
            errs.append(f"应命中未命中: {rid}（实际命中: {sorted(hits)}）")
    for rid in a.get("not_hit", []):
        if rid in hits:
            errs.append(f"不应命中却命中: {rid}")
    if "hit_either" in a:
        if not set(a["hit_either"]) & hits:
            errs.append(f"应至少命中其一: {a['hit_either']}（实际: {sorted(hits)}）")
    if "hit_or_insufficient" in a:
        ok = set(a["hit_or_insufficient"]) & hits or any(
            rid in skipped
            and skipped[rid].get("status") in ("SKIPPED", "INSUFFICIENT_DATA")
            for rid in a["hit_or_insufficient"]
        )
        if not ok:
            errs.append(f"应命中或因证据不足跳过: {a['hit_or_insufficient']}")
    for rid in a.get("skipped_contains", []):
        if rid not in skipped:
            errs.append(f"应出现在 skipped: {rid}")
    for rid, frag in a.get("skipped_reason_contains", {}).items():
        entry = skipped.get(rid)
        if entry is None:
            errs.append(f"应出现在 skipped: {rid}（reason 断言无从检查）")
        elif frag not in (entry.get("reason") or ""):
            errs.append(
                f"skipped[{rid}].reason 应含 {frag!r}，实际 {entry.get('reason')!r}"
            )
    if "severity" in a:
        hit_map = {f["rule_id"]: f for f in v["findings"]}
        for rid, want in a["severity"].items():
            got = hit_map.get(rid, {}).get("severity")
            if got != want:
                errs.append(f"{rid} severity 应为 {want}，实际 {got}")
    if "counter_evidence_hit" in a:
        hit_map = {f["rule_id"]: f for f in v["findings"]}
        for rid, ce_ids in a["counter_evidence_hit"].items():
            ces = {c["id"]: c for c in hit_map.get(rid, {}).get("counter_evidence", [])}
            for ce_id in ce_ids:
                if ce_id not in ces:
                    errs.append(f"{rid} 反证 {ce_id} 缺失")
                elif ces[ce_id].get("unevaluated"):
                    errs.append(f"{rid} 反证 {ce_id} 应已评估（实际 unevaluated）")
    if "counter_evidence_unevaluated" in a:
        hit_map = {f["rule_id"]: f for f in v["findings"]}
        for rid, ce_ids in a["counter_evidence_unevaluated"].items():
            ces = {c["id"]: c for c in hit_map.get(rid, {}).get("counter_evidence", [])}
            for ce_id in ce_ids:
                if ce_id not in ces:
                    errs.append(f"{rid} 反证 {ce_id} 缺失")
                elif not ces[ce_id].get("unevaluated"):
                    errs.append(f"{rid} 反证 {ce_id} 应为 unevaluated（缺失语义）")
    if "facts_pack_contains" in a:
        facts = v.get("facts", {})
        for pack, metrics in a["facts_pack_contains"].items():
            if pack not in facts:
                errs.append(f"facts 应含包层 {pack}（实际键: {list(facts)}）")
                continue
            for m in metrics:
                if m not in facts[pack]:
                    errs.append(f"facts[{pack}] 应含指标 {m}")
    if "verdict" in a and v["verdict"] != a["verdict"]:
        errs.append(f"verdict 应为 {a['verdict']}，实际 {v['verdict']}")
    if "rows" in a and v.get("rows") != a["rows"]:
        errs.append(f"rows 应为 {a['rows']}，实际 {v.get('rows')}")

    if verbose:
        print(
            f"    hits={sorted(hits)}  verdict={v['verdict']}  headline={v['headline']}"
        )
    return (not errs), errs


def main() -> int:
    verbose = "-v" in sys.argv
    passed, failed = 0, []
    for case in CASES + SCRIPT_CASES:
        runner = run_case if case.get("args") is not None else run_script_case
        ok, errs = runner(case, verbose)
        mark = "PASS" if ok else "FAIL"
        print(f"[{mark}] {case['id']}: {case['desc']}")
        if ok:
            passed += 1
        else:
            failed.append(case["id"])
            for e in errs:
                print(f"       ✗ {e}")
    print(f"\n{passed}/{len(CASES) + len(SCRIPT_CASES)} 通过")
    if failed:
        print(
            f"失败用例: {', '.join(failed)} —— 规则/阈值变更未过回归，禁止合入（阈值纪律）"
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
