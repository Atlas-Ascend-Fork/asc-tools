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
"""batch_pipeline.py — 全量 case 批量快照与对比（检视 F11）

把 A1 全量 case 铁律（>20 取前 20 逐 case 覆盖）从「主 agent 手工串 20×N
次命令再手拼表」变成一条命令：输入 cases.csv + 逐 case 性能数据 +
基线快照，自动逐 case 建 snapshot → 跑 delta_compare → 输出
comparison_matrix.md（逐 case 表 + geomean + 最差/最好 case + 精度列）。

输入形态（性能数据 CSV，一行一 case×variant）：
  case_id,variant,kernel_avg_us,precision[,kernel_p50_us,kernel_p95_us,kernel_max_us]
  c01,baseline,100.0,pass
  c01,round-01-ub-fusion,92.0,pass

用法：
  python3 batch_pipeline.py \
      --cases cases.csv \
      --perf perf_data.csv \
      --baseline-variant baseline \
      --after-variants round-01-ub-fusion,round-01-mte2-preload \
      --scope tuning_artifacts/gelu/scope.json \
      --output-dir tuning_artifacts/gelu/matrix

  # 直连模式（P0-1）：collect_cases.py 产物目录直接进来，无需手拼 CSV
  python3 batch_pipeline.py \
      --from-collect ./collect_out \
      --cases cases.csv \
      --baseline-variant system \
      --after-variants custom \
      --scope tuning_artifacts/gelu/scope.json \
      --output-dir tuning_artifacts/gelu/matrix
  # --from-collect 读取 <dir>/perf_data.csv（collect_cases.py 产出）；
  # baseline-variant=system + after-variants=custom 即「vs 标杆」读数。

依赖 scope.json（opstune.scope/1.0）提供 12 HARD 口径字段；逐 case 的
shape/dtype 从 cases.csv 透传进 snapshot（shape 是 HARD 字段，逐 case 值不同）。
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SNAPSHOT = HERE / "snapshot.py"
DELTA_COMPARE = HERE / "delta_compare.py"

HARD_KEYS = (
    "op",
    "soc",
    "mode",
    "timing_method",
    "baseline_kind",
    "cann_version",
    "device_id",
    "shape",
    "dtype",
    "format",
    "warmup",
    "repeat",
)


def load_cases(path: str) -> list[dict]:
    with open(path, encoding="utf-8-sig") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        if not r.get("case_id") or not r.get("shape") or not r.get("dtype"):
            raise SystemExit(f"ERROR: cases.csv 行缺必需列（case_id/shape/dtype）: {r}")
    return rows


def load_perf(path: str) -> dict[tuple[str, str], dict]:
    """{(case_id, variant): {kernel_avg_us, precision, 分布字段...}}

    kernel_avg_us 为空（collect 侧采集失败/无数据）的行跳过并 WARN——
    直连模式下 collect_summary 已有状态，不在此静默编造。"""
    out = {}
    with open(path, encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            key = (row["case_id"], row["variant"])
            if not str(row.get("kernel_avg_us", "")).strip():
                print(f"WARN: perf 行无 kernel_avg_us，跳过: {key}", file=sys.stderr)
                continue
            out[key] = {
                "kernel_avg_us": float(row["kernel_avg_us"]),
                "precision": row.get("precision", "pass"),
                "kernel_p50_us": float(row["kernel_p50_us"])
                if row.get("kernel_p50_us")
                else None,
                "kernel_p95_us": float(row["kernel_p95_us"])
                if row.get("kernel_p95_us")
                else None,
                "kernel_max_us": float(row["kernel_max_us"])
                if row.get("kernel_max_us")
                else None,
            }
    return out


def run_snapshot(
    scope: dict, case: dict, variant: str, perf: dict, out_path: Path
) -> None:
    cmd = [
        sys.executable,
        str(SNAPSHOT),
        "--op",
        scope["op"],
        "--variant",
        variant,
        "--mode",
        scope["mode"],
        "--soc",
        scope["soc"],
        "--precision",
        perf["precision"],
        "--kernel-avg-us",
        str(perf["kernel_avg_us"]),
        "--timing-method",
        scope["timing_method"],
        "--baseline-kind",
        scope["baseline_kind"],
        "--cann-version",
        scope["cann_version"],
        "--device-id",
        scope["device_id"],
        "--shape",
        case["shape"],
        "--dtype",
        case["dtype"],
        "--format",
        scope["format"],
        "--warmup",
        str(scope["warmup"]),
        "--repeat",
        str(scope["repeat"]),
        "--no-env-probe",
        "-o",
        str(out_path),
    ]
    for opt, key in (
        ("--kernel-p50-us", "kernel_p50_us"),
        ("--kernel-p95-us", "kernel_p95_us"),
        ("--kernel-max-us", "kernel_max_us"),
    ):
        if perf.get(key) is not None:
            cmd += [opt, str(perf[key])]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"ERROR: snapshot 失败 {out_path}: {proc.stderr[:300]}")


def run_compare(base: Path, after: Path, out_md: Path) -> tuple[int, str]:
    proc = subprocess.run(
        [
            sys.executable,
            str(DELTA_COMPARE),
            "--baseline",
            str(base),
            "--after",
            str(after),
            "--output",
            str(out_md),
        ],
        capture_output=True,
        text=True,
    )
    return proc.returncode, proc.stdout


def parse_conclusion(md_text: str) -> tuple[str, float | None]:
    """从 comparison.md 提取三段结论类型与 speedup。"""
    speedup = None
    for line in md_text.splitlines():
        # 行形态：| speedup | — | 1.200 |（第三列为值）
        if line.startswith("| speedup |"):
            cells = [c.strip() for c in line.split("|")]
            try:
                speedup = float(cells[3])
            except (ValueError, IndexError):
                pass
    if "提升" in md_text:
        return "KEEP", speedup
    if "劣化" in md_text:
        return "REVERT", speedup
    if "噪声带内" in md_text:
        return "NOISE", speedup
    return "UNRESOLVED", speedup


def main() -> int:
    ap = argparse.ArgumentParser(
        description="全量 case 批量快照与对比（A1 铁律工具化）"
    )
    ap.add_argument(
        "--cases", required=True, help="cases.csv（case_id/shape/dtype/attrs）"
    )
    ap.add_argument(
        "--perf",
        default=None,
        help="性能数据 CSV（case_id/variant/kernel_avg_us/precision[分布字段]）；"
        "与 --from-collect 二选一",
    )
    ap.add_argument(
        "--from-collect",
        default=None,
        help="collect_cases.py 产物根目录（P0-1 直连：读 <dir>/perf_data.csv，"
        "消灭手拼 CSV；未完成的 case 行由 collect 侧标注，此处如实跳过）",
    )
    ap.add_argument("--baseline-variant", default="baseline")
    ap.add_argument("--after-variants", required=True, help="逗号分隔的优化变体名")
    ap.add_argument(
        "--scope",
        required=True,
        help="scope.json（opstune.scope/1.0，提供 12 HARD 字段）",
    )
    ap.add_argument("--output-dir", required=True)
    ap.add_argument(
        "--max-cases",
        type=int,
        default=20,
        help="case 截断上限（A1 铁律 >20 取前 20；0 = 不截断）",
    )
    args = ap.parse_args()

    scope = json.loads(Path(args.scope).read_text(encoding="utf-8"))
    missing = [k for k in HARD_KEYS if k not in scope]
    if missing:
        print(f"ERROR: scope.json 缺 HARD 字段 {missing}", file=sys.stderr)
        return 1

    cases = load_cases(args.cases)
    truncated = False
    if args.max_cases and len(cases) > args.max_cases:
        cases = cases[: args.max_cases]
        truncated = True
        print(
            f"WARN: cases {len(load_cases(args.cases))} > {args.max_cases}，取前 {args.max_cases}"
            f"（A1 铁律截断，报告必须告知用户）",
            file=sys.stderr,
        )

    if not args.perf and not args.from_collect:
        print("ERROR: --perf 与 --from-collect 必须提供其一", file=sys.stderr)
        return 1
    if args.perf and args.from_collect:
        print(
            "ERROR: --perf 与 --from-collect 互斥（直连模式自带 perf_data.csv）",
            file=sys.stderr,
        )
        return 1
    perf_path = args.perf
    if args.from_collect:
        collect_root = Path(args.from_collect)
        candidate = collect_root / "perf_data.csv"
        if not candidate.exists():
            print(
                f"ERROR: {candidate} 不存在（--from-collect 需要 collect_cases.py 产物目录）",
                file=sys.stderr,
            )
            return 1
        perf_path = str(candidate)
        # 伪 Green 防护：collect 侧 failed/partial 如实透传（不静默跳过）
        summary_path = collect_root / "collect_summary.json"
        if summary_path.exists():
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
            bad = summary.get("failed_or_partial", [])
            if bad:
                print(
                    f"WARN: collect 侧 {len(bad)} 项 failed/partial 将无数据（详见 "
                    f"{summary_path}，伪 Green 防护不抹掉）",
                    file=sys.stderr,
                )

    perf = load_perf(perf_path)
    variants = [v.strip() for v in args.after_variants.split(",") if v.strip()]
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # ---- 逐 case × 逐变体：snapshot + delta_compare ----
    matrix: dict[tuple[str, str], dict] = {}
    for case in cases:
        cid = case["case_id"]
        base_perf = perf.get((cid, args.baseline_variant))
        if base_perf is None:
            print(f"WARN: case {cid} 无基线数据，跳过", file=sys.stderr)
            continue
        base_snap = out_dir / cid / "baseline.json"
        run_snapshot(scope, case, args.baseline_variant, base_perf, base_snap)
        for variant in variants:
            v_perf = perf.get((cid, variant))
            if v_perf is None:
                print(
                    f"WARN: case {cid} 变体 {variant} 无数据，记 MISSING",
                    file=sys.stderr,
                )
                matrix[(cid, variant)] = {
                    "status": "MISSING",
                    "speedup": None,
                    "precision": "-",
                    "base_us": base_perf["kernel_avg_us"],
                    "after_us": None,
                }
                continue
            v_snap = out_dir / cid / f"{variant}.json"
            comp_md = out_dir / cid / f"{variant}_comparison.md"
            run_snapshot(scope, case, variant, v_perf, v_snap)
            rc, stdout = run_compare(base_snap, v_snap, comp_md)
            if rc == 2:
                matrix[(cid, variant)] = {
                    "status": "REJECTED(口径/精度)",
                    "speedup": None,
                    "precision": v_perf["precision"],
                    "base_us": base_perf["kernel_avg_us"],
                    "after_us": v_perf["kernel_avg_us"],
                }
                continue
            status, speedup = parse_conclusion(comp_md.read_text(encoding="utf-8"))
            matrix[(cid, variant)] = {
                "status": status,
                "speedup": speedup,
                "precision": v_perf["precision"],
                "base_us": base_perf["kernel_avg_us"],
                "after_us": v_perf["kernel_avg_us"],
            }
        # 中间快照保留策略（P2-2/G20 ③，反馈修正：此前注释自相矛盾——标题「清理」
        # 正文「不删」）。决策：**默认全保留**（snapshot 契约文件是审计真源，
        # delta_compare 的口径指纹复验依赖两侧 json）；目录膨胀用归档清单治理：
        # comparison_matrix.md 末尾自动生成产物索引（哪些文件可归档/删除由人工
        # 按清单决策，不自动删除——误删审计链的代价 > 磁盘成本）。

    # ---- comparison_matrix.md ----
    md = [
        "# 全量 case 对比矩阵（batch_pipeline 自动生成）",
        "",
        f"- 基线变体：{args.baseline_variant}；优化变体：{', '.join(variants)}",
        f"- case 数：{len(cases)}{'（已按 A1 铁律截断，须告知用户）' if truncated else ''}",
        f"- 口径：{scope['soc']} / CANN {scope['cann_version']} / device {scope['device_id']}"
        f" / timing={scope['timing_method']} / warmup={scope['warmup']} repeat={scope['repeat']}",
        "",
    ]
    for variant in variants:
        md += [
            f"## {variant}",
            "",
            "| Case | Shape | dtype | 基线(us) | 优化后(us) | speedup | 结论 | 精度 |",
            "|------|-------|-------|---------|-----------|---------|------|------|",
        ]
        speeds = []
        for case in cases:
            m = matrix.get((case["case_id"], variant))
            if m is None:
                continue
            sp = f"{m['speedup']:.3f}x" if m["speedup"] else "—"
            md.append(
                f"| {case['case_id']} | {case['shape']} | {case['dtype']} | "
                f"{m['base_us']} | {m['after_us'] if m['after_us'] else '—'} | "
                f"{sp} | {m['status']} | {m['precision']} |"
            )
            if m["speedup"]:
                speeds.append(m["speedup"])
        md.append("")
        if speeds:
            geomean = math.exp(sum(math.log(s) for s in speeds) / len(speeds))
            best = max(speeds)
            worst = min(speeds)
            n_keep = sum(
                1
                for c in cases
                if matrix.get((c["case_id"], variant), {}).get("status") == "KEEP"
            )
            n_revert = sum(
                1
                for c in cases
                if matrix.get((c["case_id"], variant), {}).get("status") == "REVERT"
            )
            n_noise = sum(
                1
                for c in cases
                if matrix.get((c["case_id"], variant), {}).get("status") == "NOISE"
            )
            n_rej = sum(
                1
                for c in cases
                if str(
                    matrix.get((c["case_id"], variant), {}).get("status", "")
                ).startswith("REJECTED")
            )
            md += [
                "**汇总**：",
                f"- geomean speedup：**{geomean:.3f}x**（{len(speeds)}/{len(cases)} case 有可比数据）",
                f"- 最好 case：{best:.3f}x ｜ 最差 case：{worst:.3f}x",
                f"- KEEP {n_keep} ｜ REVERT {n_revert} ｜ 噪声带内 {n_noise} ｜ 拒绝比较 {n_rej} ｜ MISSING {len(cases) - len(speeds) - n_rej}",
                "",
                "> 汇总口径：拒绝比较（口径漂移/精度 fail）与 MISSING 的 case 不进 geomean；"
                "REVERT case 计入（负样本是瓶颈证据）。逐 case comparison.md 见各 case 子目录。",
            ]
        md.append("")

    # ---- 产物索引（P2-2/G20 ③：目录膨胀治理——归档清单自动生成，删除由人工决策）----
    n_snap = n_comp = 0
    for cid_dir in out_dir.iterdir():
        if not cid_dir.is_dir():
            continue
        n_snap += sum(1 for p in cid_dir.glob("*.json"))
        n_comp += sum(1 for p in cid_dir.glob("*_comparison.md"))
    md += [
        "## 产物索引（tuning_artifacts 归档清单）",
        "",
        f"- 逐 case snapshot JSON：{n_snap} 份（口径指纹审计真源，delta_compare 复验依赖）",
        f"- 逐 case comparison.md：{n_comp} 份（三段结论与处置依据）",
        "- 本矩阵：comparison_matrix.md",
        "",
        "> 生命周期：多轮迭代结束后按本清单人工决策归档/删除（ KEEP 轮的两侧 json",
        "> 与 REVERT 轮的 comparison.md 建议长期保留——回滚记录是瓶颈证据；",
        "> NOISE 轮中间快照可归档压缩）。本工具不自动删除任何产物。",
    ]

    out_md = out_dir / "comparison_matrix.md"
    out_md.write_text("\n".join(md) + "\n", encoding="utf-8")
    n_rows = sum(1 for k in matrix if k[1] in variants)
    print(
        f"matrix → {out_md}（{n_rows} 行 = {len(cases)} case × {len(variants)} 机制；"
        f"产物索引：{n_snap} snapshot + {n_comp} comparison）"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
