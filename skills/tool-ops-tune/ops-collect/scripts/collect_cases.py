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
"""collect_cases.py — 逐 case 采集编排 + 标杆双路径（REVIEW2 G1+G5 / 整改 P0-1）

设计对齐：
  - A1 全量 case 铁律（>20 取前 20 逐 case 覆盖）此前靠主 agent 手工串
    20×N 次 collect_board.sh 再手拼 perf CSV——三类高频错误（漏 case 违 A1、
    口径漂移触发 delta_compare exit 2 后双侧重采、目录混乱）全在这一步。
  - baseline_kind=system 此前只是 snapshot 的一个字段，无采集编排——「比标杆
    快多少」（用户终极问题）不可答。
  - 本脚本把两者合一：输入 cases.csv + exe，逐 case × 逐 variant（内建
    {custom, system} 双路径）调 collect_board.sh，产出 batch_pipeline.py
    直接可吃的 perf_data.csv + 逐 case manifest 归档。

用法：
  # 单算子（自前后对比）：
  python3 collect_cases.py --cases cases.csv --exe ./build/gelu_demo \\
      --mode quick --output ./collect_out

  # 标杆双路径（cases.csv 扩展列 ref_impl = 标杆可执行/描述）：
  python3 collect_cases.py --cases cases.csv --exe ./build/gelu_demo \\
      --ref-exe "python3 ref/gelu_ref.py" --mode quick --output ./collect_out

  # 定向补采（只重采指定 case）：
  python3 collect_cases.py --cases cases.csv --exe ./build/gelu_demo \\
      --mode quick --output ./collect_out --cases-subset c03,c07

产物布局（{output}/{case_id}/{variant}/ 每目录一份 collection_manifest）：
  collect_out/
    perf_data.csv                      ← batch_pipeline --perf 直接输入
    collect_summary.json               ← 逐 case × variant 状态（伪 Green 防护）
    c01/custom/collection_manifest.json
    c01/system/collection_manifest.json
    ...

纪律继承：
  - case 间串行、同卡同口径（device 由 collect_board.sh pick_device 统一选择
    并注入 ASCEND_RT_VISIBLE_DEVICES）；
  - 已有 manifest 的目录跳过（防重采闸门同款——补采用 --cases-subset 指定）；
  - 采集失败不静默：逐条记入 collect_summary.json 的 failed 列表。
"""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
import time
from pathlib import Path

SCHEMA = "opstune.collect-cases/1.0"
HERE = Path(__file__).resolve().parent
COLLECT_BOARD = HERE / "collect_board.sh"


def load_cases(path: str) -> list[dict]:
    with open(path, encoding="utf-8-sig") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        if not r.get("case_id") or not r.get("shape") or not r.get("dtype"):
            raise SystemExit(f"ERROR: cases.csv 行缺必需列（case_id/shape/dtype）: {r}")
    return rows


def run_collect(
    exe: str,
    args_str: str,
    mode: str,
    out_dir: Path,
    timeout: int,
    kernel_name: str | None,
) -> dict:
    """调 collect_board.sh 一次；返回 {status, manifest, task_duration_us}。"""
    cmd = [
        "bash",
        str(COLLECT_BOARD),
        "--exe",
        exe,
        "--mode",
        mode,
        "--output",
        str(out_dir),
        "--timeout",
        str(timeout),
    ]
    if args_str:
        cmd += ["--args", args_str]
    if kernel_name:
        cmd += ["--kernel-name", kernel_name]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    manifest_path = out_dir / "collection_manifest.json"
    manifest = None
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if proc.returncode != 0:
        return {
            "status": "failed",
            "rc": proc.returncode,
            "stderr": proc.stderr[-400:],
            "manifest": manifest,
        }
    if manifest is None:
        return {
            "status": "failed",
            "rc": proc.returncode,
            "stderr": "collect_board 退出 0 但无 manifest",
            "manifest": None,
        }
    blocks_ok = all(b.get("status") == "ok" for b in manifest.get("blocks", []))
    return {
        "status": "ok" if blocks_ok else "partial",
        "rc": 0,
        "stderr": "",
        "manifest": manifest,
    }


def task_duration_of(manifest: dict | None, out_dir: Path) -> float | None:
    """从采集产物提取 Task Duration 均值（quick/standard 模式的 op_summary）。"""
    if manifest is None:
        return None
    # 快路径：timing 模式 result.json（P0-2 产物）
    timing_json = out_dir / "result.json"
    if timing_json.exists():
        try:
            t = json.loads(timing_json.read_text(encoding="utf-8"))
            if t.get("kernel_avg_us") is not None:
                return float(t["kernel_avg_us"])
        except (json.JSONDecodeError, ValueError):
            pass
    # op_summary 路径：全部 op_summary Task Duration 均值（单 kernel 语义）
    vals: list[float] = []
    for p in sorted(out_dir.rglob("op_summary_*.csv")):
        with open(p, encoding="utf-8-sig", errors="replace") as f:
            for row in csv.DictReader(f):
                v = row.get("Task Duration(us)") or row.get("Task Duration")
                if v:
                    try:
                        vals.append(float(str(v).strip()))
                    except ValueError:
                        pass
    return (sum(vals) / len(vals)) if vals else None


def main() -> int:
    ap = argparse.ArgumentParser(
        description="逐 case 采集编排 + 标杆双路径（A1 铁律工具化）"
    )
    ap.add_argument(
        "--cases",
        required=True,
        help="cases.csv（case_id/shape/dtype/attrs[/ref_impl]）",
    )
    ap.add_argument("--exe", required=True, help="被测可执行文件（custom 路径）")
    ap.add_argument(
        "--exe-args",
        default="",
        help="exe 追加参数模板（{shape} {dtype} {attrs} 占位符逐 case 替换）",
    )
    ap.add_argument(
        "--ref-exe",
        default=None,
        help="标杆路径/命令（system 路径；缺省无标杆，仅 custom 单路径）",
    )
    ap.add_argument(
        "--ref-exe-args", default=None, help="标杆参数模板（缺省同 --exe-args）"
    )
    ap.add_argument(
        "--mode",
        default="quick",
        choices=["quick", "standard", "timing"],
        help="采集模式：quick=时间复核 / standard=7 组指标（判定输入）/ timing=event harness",
    )
    ap.add_argument("--output", required=True, help="采集根目录")
    ap.add_argument("--timeout", type=int, default=1800)
    ap.add_argument(
        "--kernel-name", default=None, help="多 kernel 程序过滤（透传 collect_board）"
    )
    ap.add_argument(
        "--max-cases",
        type=int,
        default=20,
        help="case 截断上限（A1 铁律 >20 取前 20；0 = 不截断）",
    )
    ap.add_argument(
        "--cases-subset",
        default="",
        help="定向补采：逗号分隔 case_id 列表（只采这些 case）",
    )
    args = ap.parse_args()

    cases = load_cases(args.cases)
    if args.max_cases and len(cases) > args.max_cases:
        cases = cases[: args.max_cases]
        print(
            f"WARN: cases 截断为前 {args.max_cases}（A1 铁律，报告必须告知用户）",
            file=sys.stderr,
        )
    if args.cases_subset:
        wanted = {c.strip() for c in args.cases_subset.split(",") if c.strip()}
        cases = [c for c in cases if c["case_id"] in wanted]
        if not cases:
            print(
                f"ERROR: --cases-subset 无匹配 case（{sorted(wanted)}）",
                file=sys.stderr,
            )
            return 1

    variants = [("custom", args.exe, args.exe_args)]
    if args.ref_exe:
        variants.append(
            (
                "system",
                args.ref_exe,
                args.ref_exe_args if args.ref_exe_args is not None else args.exe_args,
            )
        )

    out_root = Path(args.output)
    out_root.mkdir(parents=True, exist_ok=True)

    # ---- 逐 case × 逐 variant 串行采集（同卡同口径；已有 manifest 跳过）----
    perf_rows: list[dict] = []
    status_rows: list[dict] = []
    t0 = time.time()
    for case in cases:
        cid = case["case_id"]
        tpl_vars = {
            "shape": case["shape"],
            "dtype": case["dtype"],
            "attrs": case.get("attrs", ""),
            "case_id": cid,
        }
        for vname, exe, args_tpl in variants:
            if (
                vname == "system"
                and not case.get("ref_impl", "").strip()
                and args_tpl == ""
            ):
                # cases.csv 无 ref_impl 列且未给参数模板：标杆跑与 custom 相同命令无意义
                if not case.get("ref_impl"):
                    status_rows.append(
                        {
                            "case_id": cid,
                            "variant": vname,
                            "status": "skipped",
                            "reason": "cases.csv 无 ref_impl（该 case 无标杆定义）",
                        }
                    )
                    continue
            vdir = out_root / cid / vname
            if (vdir / "collection_manifest.json").exists():
                dur = task_duration_of(None, vdir)
                perf_rows.append(
                    {
                        "case_id": cid,
                        "variant": vname,
                        "kernel_avg_us": dur if dur else "",
                        "precision": "",
                    }
                )
                status_rows.append(
                    {
                        "case_id": cid,
                        "variant": vname,
                        "status": "reused",
                        "reason": "已有 manifest（防重采闸门），直接复用",
                    }
                )
                continue
            exe_args = args_tpl.format(**tpl_vars) if args_tpl else ""
            res = run_collect(
                exe, exe_args, args.mode, vdir, args.timeout, args.kernel_name
            )
            dur = task_duration_of(res.get("manifest"), vdir)
            perf_rows.append(
                {
                    "case_id": cid,
                    "variant": vname,
                    "kernel_avg_us": dur if dur is not None else "",
                    "precision": "",
                }
            )
            status_rows.append(
                {
                    "case_id": cid,
                    "variant": vname,
                    "status": res["status"],
                    "reason": res.get("stderr", "")[:200]
                    if res["status"] != "ok"
                    else "",
                }
            )
            mark = res["status"].upper()
            print(
                f"  [{mark:>7}] {cid}/{vname}"
                f"{'  task_duration=%.1fus' % dur if dur else ''}"
            )

    # ---- perf_data.csv（batch_pipeline --perf 直接输入格式）----
    perf_path = out_root / "perf_data.csv"
    with open(perf_path, "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(
            f, fieldnames=["case_id", "variant", "kernel_avg_us", "precision"]
        )
        w.writeheader()
        for r in perf_rows:
            w.writerow(
                {**r, "precision": r["precision"] or "pass"}
            )  # 精度列由 Phase 4 实施后填真值

    # ---- collect_summary.json（伪 Green 防护：failed/partial 如实呈现）----
    failed = [s for s in status_rows if s["status"] in ("failed", "partial")]
    summary = {
        "schema": SCHEMA,
        "cases": len(cases),
        "variants": [v for v, _, _ in variants],
        "mode": args.mode,
        "elapsed_s": round(time.time() - t0, 1),
        "ok": len([s for s in status_rows if s["status"] in ("ok", "reused")]),
        "failed_or_partial": failed,
        "rows": status_rows,
        "next": (
            f"python3 ../ops-verdict/scripts/batch_pipeline.py "
            f"--from-collect {out_root} ...（直连模式，无需手拼 CSV）"
        ),
    }
    (out_root / "collect_summary.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )

    print(
        f"perf_data.csv → {perf_path}（{len(perf_rows)} 行 = {len(cases)} case × {len(variants)} variant）"
    )
    if failed:
        print(
            f"⚠ {len(failed)} 项 failed/partial（详见 collect_summary.json）——伪 Green 防护：不得从清单抹掉",
            file=sys.stderr,
        )
    print(
        f"下一步：batch_pipeline.py --from-collect {out_root} --baseline-variant "
        f"{'system' if len(variants) > 1 else 'custom'} ..."
    )
    return 1 if any(s["status"] == "failed" for s in status_rows) else 0


if __name__ == "__main__":
    sys.exit(main())
