#!/usr/bin/env bash
# -----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# -----------------------------------------------------------------------------------------------------------
# collect_sim.sh — 仿真路径采集编排（REVIEW2 G7 / 反馈 P1-1①）
#
# 设计对齐：
#   - tool-routing 决策树④「无卡 / 指令级下钻 → cannsim（仅 A5 单卡 AI Core）」
#     此前只有决策树一行文字，全仓无 cannsim 编排脚本——mode=sim /
#     timing_method=sim 空转（REVIEW2 G7 定性）。
#   - 本脚本补编排：cannsim record（精度+性能仿真）→ report（trace 生成）→
#     归档进 collection_manifest（mode=sim 块），下游 trace_bubble.py /
#     verdict.py 消费同一条链。
#
# 用法：
#   bash collect_sim.sh --exe ./build/demo --args "8448 4096 4096" \
#       --soc-version Ascend950 --output ./prof/sim_base
#   可选：--timeout <秒>（默认 1800）；--cores <0 | 0-2 | all>（report 核范围，默认 0）
#
# 产物布局（归档进 manifest，与上板采集同构）：
#   {output}/cannsim_<timestamp>_<app>/   cannsim 原始输出（cannsim.log 等）
#   {output}/report/                      cannsim report 输出（trace_core*.json）
#   {output}/collection_manifest.json     mode=sim 的采集块真源
#
# 门禁（先探测再选命令，与 collect_board.sh 同款纪律）：
#   cannsim 不可用 → 明确报错（不静默降级）；--soc-version 必填（仿真目标芯片）。

set -euo pipefail

EXE=""
EXE_ARGS=""
SOC_VERSION=""
OUTPUT="./prof/sim_run"
CORES="0"
TIMEOUT=1800

usage() {
  sed -n '2,22p' "$0"; exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --exe) EXE="$2"; shift 2 ;;
    --args) EXE_ARGS="$2"; shift 2 ;;
    --soc-version) SOC_VERSION="$2"; shift 2 ;;
    --output) OUTPUT="$2"; shift 2 ;;
    --cores) CORES="$2"; shift 2 ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

[[ -x "$EXE" || -f "$EXE" ]] || { echo "ERROR: --exe 不存在或不可执行: $EXE" >&2; exit 1; }
[[ -n "$SOC_VERSION" ]] || { echo "ERROR: --soc-version 必填（仿真目标芯片，如 Ascend950）" >&2; exit 1; }

# ---- 工具探测（cannsim 是 python 入口，PATH 或 CANN python 路径均可）----
CANNBIN=""
if command -v cannsim >/dev/null 2>&1; then
  CANNBIN="cannsim"
elif [[ -n "${ASCEND_HOME_PATH:-}" && -x "${ASCEND_HOME_PATH}/bin/cannsim" ]]; then
  CANNBIN="${ASCEND_HOME_PATH}/bin/cannsim"
else
  echo "ERROR: cannsim 不可用。检查 CANN 安装（simulation 组件）与 source set_env.sh。" >&2
  echo "       cannsim 仅 A5 单卡 AI Core 仿真；MC²/HCCL 不支持（tool-routing 决策树④）。" >&2
  exit 1
fi
echo "[probe] cannsim: $CANNBIN"

mkdir -p "$OUTPUT"
MANIFEST="$OUTPUT/collection_manifest.json"
if [[ -f "$MANIFEST" ]]; then
  echo "ERROR: $MANIFEST 已存在 —— 该目录只允许解析，禁止重复采集（与上板同款闸门）。" >&2
  exit 2
fi

BLOCKS=()
record_block() {  # name status path note
  BLOCKS+=("$(python3 -c 'import json,sys; print(json.dumps({"name": sys.argv[1], "status": sys.argv[2], "path": sys.argv[3], "note": sys.argv[4]}, ensure_ascii=False))' "$1" "$2" "$3" "$4")")
}

# ---- cannsim record（精度+性能仿真，-g 顺带生成 report）----
echo "[sim] cannsim record（soc=$SOC_VERSION）…"
if timeout "$TIMEOUT" "$CANNBIN" record "$EXE" -s "$SOC_VERSION" -o "$OUTPUT" -g \
     ${EXE_ARGS:+-u "$EXE_ARGS"} > "$OUTPUT/cannsim_record.log" 2>&1; then
  # cannsim 输出目录形态：{output}/cannsim_<timestamp>_<appname>/
  SIM_DIR=$(ls -dt "$OUTPUT"/cannsim_* 2>/dev/null | head -1 || true)
  if [[ -n "${SIM_DIR:-}" && -d "$SIM_DIR/report" ]]; then
    record_block "cannsim_record" ok "$SIM_DIR" "soc=$SOC_VERSION args=${EXE_ARGS:-<none>}"
    record_block "cannsim_report" ok "$SIM_DIR/report" "trace_core*.json（Chrome Trace Format）"
  else
    record_block "cannsim_record" partial "$OUTPUT" \
      "record 退出 0 但未发现 cannsim_*/report 目录（检查 cannsim_record.log）"
  fi
else
  rc=$?
  record_block "cannsim_record" failed "$OUTPUT" \
    "cannsim record 退出非 0（rc=$rc），日志: $OUTPUT/cannsim_record.log"
fi

# ---- manifest 落盘（mode=sim；与上板同构，下游 facts_bridge/verdict 同链消费）----
BLOCKS_JSON=$(printf '\x1f%s' "${BLOCKS[@]:-}")
BLOCKS_JSON=${BLOCKS_JSON:1}
python3 - "$MANIFEST" "$EXE" "$EXE_ARGS" "$SOC_VERSION" "$CORES" "$BLOCKS_JSON" <<'PYEOF'
import json, sys, datetime
manifest = {
    "schema": "opstune.collection/1.0",
    "generated_at": datetime.datetime.now().isoformat(timespec="seconds"),
    "mode": "sim",
    "exe": sys.argv[2],
    "args": sys.argv[3],
    "soc_version": sys.argv[4],
    "report_cores": sys.argv[5],
    "blocks": [json.loads(b) for b in sys.argv[6].split("\x1f")] if sys.argv[6] else [],
}
with open(sys.argv[1], "w", encoding="utf-8") as f:
    json.dump(manifest, f, ensure_ascii=False, indent=2)
    f.write("\n")
PYEOF

echo "[done] manifest → $MANIFEST"
echo "下一步："
echo "  1. 空泡/重叠度分析：python3 ../ops-verdict/scripts/trace_bubble.py --trace <report>/trace_core0.json"
echo "  2. 仿真口径快照：snapshot.py --mode sim --timing-method sim"
echo "  3. 注意：sim 数字与上板数字禁止混比（snapshot HARD 口径；timing_method=sim 专用）"
