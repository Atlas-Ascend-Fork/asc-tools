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
# collect_board.sh — 上板采集统一入口（标准 7 组指标 / quick / MC² 多 rank）
#
# 设计对齐（docs/算子调优AGENT设计文档.md §4 总体架构）：
#   - 复用不重做：底层调 msprof（CANN 官方），不封装私有格式；产物以
#     collection_manifest 契约落盘（采集块状态机真源，M2 对接可视化管线）。
#   - 采集模式路由（决策树 + 「先探测本机帮助再选命令」门禁）：
#       MC²/fork 多 rank → 强制 msprof --aic-mode=task-based（msprof op 对 fork 行为未定义）
#       单卡算子        → msprof op（若本机支持）或 msprof 7 组指标
#   - 每次采集输出 collection_manifest.json（采集块状态机真源：ok/partial/failed/skipped），
#     禁止「重采已有 ok 块」——已有 manifest 的目录只允许重解析，不允许重复采集。
#
# 用法：
#   bash collect_board.sh --exe ./build/demo --args "8448 4096 4096" --output ./prof/baseline
#   bash collect_board.sh --exe ./build/demo --mode quick  --output ./prof/quick_check
#   bash collect_board.sh --exe ./build/timing_harness --mode timing --output ./prof/timing_base   # 跑 event 计时 harness（P0-2）
#   bash collect_board.sh --exe ./build/mc2_demo --mode mc2 --ranks 4 --output ./prof/mc2_base
#   bash collect_board.sh --exe ./build/demo --mode npu-compute --output ./prof/npc_base   # 950/A5 DB 格式采集（Roadmap #4）
#   可选：--kernel-name <名>（单卡 msprof op 分支过滤，本机 --help 为门禁）
#         --timeout <秒>（msprof 单块超时，默认 1800）
#         --sections <A,B>（npu-compute 模式追加 Section，如 PmSampling——本机
#           npu-compute --list-sections 为门禁，不支持者跳过并告警）
#   注：稳态 warm-up 由被测程序自身循环保证（循环 ≥10 次前 5 warm-up），
#       采集脚本不做应用层 warmup（F17：原 --warmup 为死参数，已删除）。

set -euo pipefail

MODE=standard
EXE=""
EXE_ARGS=""
OUTPUT="./prof/run"
RANKS=1
KERNEL_NAME=""
MSPROF_TIMEOUT=1800   # msprof 单块采集超时（秒）；挂起程序不再无限阻塞（检视 F23）

usage() {
  sed -n '2,32p' "$0"; exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --exe) EXE="$2"; shift 2 ;;
    --args) EXE_ARGS="$2"; shift 2 ;;
    --mode) MODE="$2"; shift 2 ;;
    -o|--output) OUTPUT="$2"; shift 2 ;;
    --timeout) MSPROF_TIMEOUT="$2"; shift 2 ;;
    --ranks) RANKS="$2"; shift 2 ;;
    --kernel-name) KERNEL_NAME="$2"; shift 2 ;;
    --sections) NPU_SECTIONS="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

[[ -x "$EXE" || -f "$EXE" ]] || { echo "ERROR: --exe 不存在或不可执行: $EXE" >&2; exit 1; }

# ---- 工具探测（先探测再选命令：本机帮助为门禁，不复制跨版本命令）----
# npu-compute 模式（Roadmap #4）：950/A5 单卡 DB 格式采集——工具探测与
# msprof 分开（不同二进制）；timing 模式两者都不依赖。
if [[ "$MODE" == "npu-compute" ]]; then
  command -v npu-compute >/dev/null 2>&1 || {
    echo "ERROR: npu-compute 不可用（本机未安装或不在 PATH）。" >&2
    echo "       npu-compute 为 950/A5 平台采集器（随本仓 npu_tools 构建）；" >&2
    echo "       非 950 环境请回退 --mode standard（msprof 7 组）路径。" >&2
    exit 1
  }
  npu-compute --list-sets >/dev/null 2>&1 || {
    echo "ERROR: npu-compute --list-sets 探测失败（工具异常）。" >&2
    exit 1
  }
fi
# P0-2：timing 模式跑 event harness 不依赖 msprof，探测跳过（harness 直跑）
if [[ "$MODE" != "timing" && "$MODE" != "npu-compute" ]] && ! command -v msprof >/dev/null 2>&1; then
  echo "ERROR: msprof 不可用。检查 CANN 安装与 source set_env.sh" >&2
  exit 1
fi
if [[ "$MODE" != "timing" && "$MODE" != "npu-compute" ]]; then
  msprof op --help >/dev/null 2>&1 && MSPROF_OP_OK=1 || MSPROF_OP_OK=0
  echo "[probe] msprof op 支持: $MSPROF_OP_OK"
  # F17：MSPROF_OP_OK 真正参与路由（tool-routing 决策树：单卡且本机 msprof op
  # 可用 → msprof op 分支；不可用 → 完整 msprof 7 组）。kernel-name 仅
  # msprof op 形态有效（本机 --help 实测），完整 msprof 透传无此参数。
  MSPROF_KERNEL_FILTER_OK=0
  if [[ $MSPROF_OP_OK -eq 1 ]] && msprof op --help 2>&1 | grep -q -- "--kernel-name"; then
    MSPROF_KERNEL_FILTER_OK=1
  fi
fi

mkdir -p "$OUTPUT"
MANIFEST="$OUTPUT/collection_manifest.json"

# ---- 已有 manifest 的目录禁止重复采集（只允许重解析）----
if [[ -f "$MANIFEST" ]]; then
  echo "ERROR: $MANIFEST 已存在 —— 该目录只允许解析（verdict.py），禁止重复采集。" >&2
  echo "       如需重新采集，请指定新的 --output 目录。" >&2
  exit 2
fi

# ---- 空闲卡选择（多卡环境尽量避开占用卡）----
# P2-2/G20 ①：结构化解析优先（npu-smi info -t usages -i <id> 的
# Aicore/Aivector Usage Rate(%) 字段），字面 "100%" 匹配退化为兜底——
# npu-smi 输出格式随版本漂移时不再误判（此前 index($0,"100%")==0 会把
# Health 列、温度 100 等任何含 100% 的行错杀，或漏判真实满载卡）。
pick_device_structured() {
  command -v npu-smi >/dev/null 2>&1 || { echo "0"; return; }
  local total dev util
  # 设备总数探测（-l 输出 "Total Count : N"；探测失败按 1 台。注：不用
  # grep -c 探测——无匹配时 grep 退 1，pipefail+errexit 会终止整个脚本）
  total=$(npu-smi info -l 2>/dev/null | sed -n 's/.*Total Count *: *\([0-9]*\).*/\1/p' | head -1)
  [[ "$total" =~ ^[0-9]+$ ]] || total=1
  for ((dev = 0; dev < total; dev++)); do
    # npu-smi usages 输出行首为 TAB（本机 25.5.0 实测）：sed 同时容忍 TAB 与空格
    util=$(npu-smi info -t usages -i "$dev" 2>/dev/null | \
           sed -n 's/^[ \t]*Aicore Usage Rate(%) *: *\([0-9]*\).*/\1/p' | head -1)
    if [[ "$util" =~ ^[0-9]+$ ]]; then
      # 结构化字段可解析：占用 < 50% 视为可用（留余量给并行流）
      if (( util < 50 )); then
        echo "$dev"; return
      fi
    else
      # 结构化解析失败（版本差异）：退化到字面启发（原实现），并在 stderr 提示
      echo "[warn] npu-smi usages 结构化解析失败（设备 $dev）——退化到 info 表字面匹配" >&2
      local fallback
      fallback=$(npu-smi info 2>/dev/null | awk -F'|' -v d="$dev" 'NR>3 && NF>3 {
        gsub(/ /,"",$2);
        if ($2 == d && index($0,"100%")==0) { print $2; exit }
      }')
      if [[ -n "$fallback" ]]; then
        echo "$fallback"; return
      fi
    fi
  done
  echo "0"
}
pick_device() {
  if command -v npu-smi >/dev/null 2>&1; then
    local dev
    dev=$(npu-smi info 2>/dev/null | awk -F'|' 'NR>3 && NF>3 {
      gsub(/ /,"",$2);
      if ($2 ~ /^[0-9]+$/ && index($0,"100%")==0) { print $2; exit }
    }')
    echo "${dev:-0}"
  else
    echo "0"
  fi
}
DEVICE=$(pick_device_structured)
# F17：选卡后注入 ASCEND_RT_VISIBLE_DEVICES——实际执行设备与 manifest 记录一致
# （口径指纹 device_id 源头保真；此前选了空闲卡却未注入，程序仍可能跑在 0 号卡）
export ASCEND_RT_VISIBLE_DEVICES="$DEVICE"
echo "[env] device=$DEVICE ranks=$RANKS mode=$MODE"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
declare -a BLOCKS=()   # manifest 采集块记录

record_block() {  # name status path note
  # 记录间以单元分隔符 \x1f 分隔（note 可含逗号/引号，最终由 json.loads 逐条解析）
  BLOCKS+=("$(python3 -c 'import json,sys; print(json.dumps({"name": sys.argv[1], "status": sys.argv[2], "path": sys.argv[3], "note": sys.argv[4]}, ensure_ascii=False))' "$1" "$2" "$3" "$4")")
}

# ---- 采后产物有效性断言（P2-1 / G18：消灭「manifest ok 但数据不可用」的伪 Green）----
# msprof 退出 0 ≠ 产物可用。断言四项（任一不满足 → 状态降级 partial，note 记原因）：
#   ① op_summary CSV 存在且非空（≥2 行 = 表头 + ≥1 数据行）
#   ② Task Duration 列存在且有 ≥1 数值（verdict/isolation 的主消费字段）
#   ③ PipeUtilization 块：pipe 比例列 ≥1 列存在（判定引擎主输入）
#   ④ OpBasicInfo 存在且含 kernel 行（多 kernel 混采时提示 --kernel-name，G15 联动）
assert_block_artifacts() {  # prof_dir metrics_name
  local dir="$1" metric="$2"
  local notes=()
  # 找 op_summary CSV（完整 msprof 布局 PROF_*/mindstudio_profiler_output/ 或顶层导出）；
  # msprof op 形态产出的是 OPPROF_*/ 下的扁平 <Metric>.csv（无 op_summary）——
  # E2E 真机验证发现（square_add@910B3，msprof 9.0）：两种布局都合法，按序探测。
  local csv
  csv=$(find "$dir" -name "op_summary_*.csv" 2>/dev/null | head -1)
  if [[ -z "$csv" ]]; then
    csv=$(find "$dir" -name "${metric}.csv" 2>/dev/null | head -1)
  fi
  if [[ -z "$csv" ]]; then
    # msprof op 形态：任意指标 CSV 存在即有产物（如 PipeUtilization.csv）
    csv=$(find "$dir" -name "PipeUtilization.csv" -o -name "OpBasicInfo.csv" 2>/dev/null | head -1)
  fi
  if [[ -z "$csv" ]]; then
    echo "产物缺失：无 op_summary_*.csv / ${metric}.csv / OPPROF 指标 CSV"
    return 1
  fi
  local rows
  rows=$(wc -l < "$csv")
  if [[ "$rows" -lt 2 ]]; then
    echo "产物空（${rows} 行 < 2：表头+数据）"
    return 1
  fi
  # Task Duration 列只在 op_summary/OpBasicInfo 形态存在；OPPROF 逐核明细 CSV
  # 无此列（时间在 OpBasicInfo.csv）——该形态下检查 OpBasicInfo 存在即可
  if grep -q "Task Duration" "$csv"; then
    :
  elif [[ -n "$(find "$dir" -name "OpBasicInfo.csv" 2>/dev/null | head -1)" ]]; then
    :
  else
    echo "Task Duration 列缺失且无 OpBasicInfo.csv（verdict/isolation 主消费字段）"
    return 1
  fi
  if [[ "$metric" == "PipeUtilization" ]] || [[ "$metric" == "msprof_op" ]]; then
    local pipe_csv
    pipe_csv=$(find "$dir" -name "PipeUtilization.csv" 2>/dev/null | head -1)
    if [[ -z "$pipe_csv" ]]; then
      pipe_csv="$csv"
    fi
    if ! grep -qE "vec_ratio|cube_ratio|mte2_ratio|VectorUtilization|CubeUtilization|Mte2Utilization" "$pipe_csv"; then
      echo "pipe 比例列缺失（判定引擎主输入，检查 $pipe_csv）"
      return 1
    fi
  fi
  return 0
}

run_msprof() {  # metrics_name aic_metrics_value extra_flags...
  # 统一 msprof 调用形态：timeout 包裹（挂起可诊断）+ 全量日志落盘副本（检视 F23：
  # 此前 >/dev/null 2>&1 把排障现场全部丢弃，troubleshooting.md 第一条就要看采集输出）
  # P2-1：msprof 退出 0 后跑产物有效性断言，断言不过 → 状态 partial（不伪 Green）
  local name="$1" metric="$2"; shift 2
  local out="$OUTPUT/PROF_${name}_${TIMESTAMP}"
  local log="$OUTPUT/msprof_${name}.log"
  if timeout "$MSPROF_TIMEOUT" msprof --output="$out" --ai-core=on --aic-metrics="$metric" \
       --task-time=on "$@" --application="${EXE} ${EXE_ARGS}" >"$log" 2>&1; then
    local assert_note
    assert_note=$(assert_block_artifacts "$out" "$name") || true
    if [[ -z "${assert_note:-}" ]]; then
      record_block "$name" ok "$out" "aic-metrics=$metric"
    else
      record_block "$name" partial "$out" "断言未过：${assert_note}（msprof 退出 0 但产物不可用——伪 Green 防护，P2-1）"
    fi
  else
    local rc=$?
    if [[ $rc -eq 124 ]]; then
      record_block "$name" failed "$out" "msprof 超时（>${MSPROF_TIMEOUT}s），日志: $log"
    else
      record_block "$name" failed "$out" "msprof 退出非 0（rc=$rc），日志: $log"
    fi
  fi
}

run_msprof_metrics() {  # metrics_name aic_metrics_value（run_msprof 的兼容薄封装）
  run_msprof "$1" "$2"
}

run_msprof_mc2() {  # task-based 多 rank 采集（MC² 专用路径）
  # MC² 多 rank：强制 task-based（msprof op 对 fork 程序行为未定义，数据不可靠）
  # 稳态纪律由运行程序自身保证：循环 ≥10 次前 5 warm-up；跨 rank 取 max 由 verdict/报告侧执行
  local out="$OUTPUT/PROF_MC2_${TIMESTAMP}"
  local log="$OUTPUT/msprof_mc2_taskbased.log"
  if timeout "$MSPROF_TIMEOUT" msprof --output="$out" --ai-core=on --aic-mode=task-based \
       --aic-metrics=PipeUtilization --task-time=on --ascendcl=on \
       --application="${EXE} ${EXE_ARGS}" >"$log" 2>&1; then
    local assert_note
    assert_note=$(assert_block_artifacts "$out" "PipeUtilization") || true
    if [[ -z "${assert_note:-}" ]]; then
      record_block "mc2_taskbased" ok "$out" "aic-mode=task-based ranks=$RANKS"
    else
      record_block "mc2_taskbased" partial "$out" "断言未过：${assert_note}（伪 Green 防护，P2-1）"
    fi
  else
    local rc=$?
    if [[ $rc -eq 124 ]]; then
      record_block "mc2_taskbased" failed "$out" "msprof 超时（>${MSPROF_TIMEOUT}s），日志: $log"
    else
      record_block "mc2_taskbased" failed "$out" "msprof 退出非 0（rc=$rc；检查 rank≤空闲卡数），日志: $log"
    fi
  fi
}

run_msprof_op() {  # msprof op 分支（单卡 + 本机支持；tool-routing 决策树 ②③）
  # F17：单卡且本机 msprof op 可用时走 msprof op（含 Roofline/KernelScale 等能力
  # 按本机帮助）；--kernel-name 仅此形态有效（多 kernel 程序必须显式过滤）
  local out="$OUTPUT/PROF_msprof_op_${TIMESTAMP}"
  local log="$OUTPUT/msprof_msprof_op.log"
  local -a cmd=(timeout "$MSPROF_TIMEOUT" msprof op --output="$out"
                --aic-metrics=PipeUtilization,ArithmeticUtilization,Memory,MemoryL0,MemoryUB,L2Cache,ResourceConflictRatio)
  if [[ -n "$KERNEL_NAME" ]]; then
    cmd+=(--kernel-name="$KERNEL_NAME")
  fi
  cmd+=("--application=${EXE} ${EXE_ARGS}")
  if "${cmd[@]}" >"$log" 2>&1; then
    local assert_note
    assert_note=$(assert_block_artifacts "$out" "msprof_op") || true
    if [[ -z "${assert_note:-}" ]]; then
      record_block "msprof_op" ok "$out" "kernel_name=${KERNEL_NAME:-<all>}"
    else
      record_block "msprof_op" partial "$out" "断言未过：${assert_note}（伪 Green 防护，P2-1）"
    fi
  else
    local rc=$?
    if [[ $rc -eq 124 ]]; then
      record_block "msprof_op" failed "$out" "msprof op 超时（>${MSPROF_TIMEOUT}s），日志: $log"
    else
      record_block "msprof_op" failed "$out" "msprof op 退出非 0（rc=$rc），日志: $log"
    fi
  fi
}

case "$MODE" in
  standard)
    # 采集模式路由（tool-routing 决策树）：MC² 强制 task-based；单卡在本机
    # msprof op 可用时优先 msprof op（F17：探测结果真正参与路由，不再只是打印）
    if [[ $RANKS -le 1 && $MSPROF_OP_OK -eq 1 ]]; then
      echo "[route] 单卡 + msprof op 可用 → msprof op 分支"
      run_msprof_op
    else
      echo "[route] 走完整 msprof 7 组 aic-metrics 顺序采集"
      for m in PipeUtilization ArithmeticUtilization Memory MemoryL0 MemoryUB L2Cache ResourceConflictRatio; do
        run_msprof_metrics "$m" "$m"
      done
    fi
    ;;
  quick)
    # 1 轮 kernel 时间（不采 7 指标）：用于快速对比 / 迭代轮次中的时间复核
    # F17 真身化：仅 task-time 轮（aic-metrics 取单组，非 7 组全量回放）
    run_msprof_metrics TaskDuration "PipeUtilization"
    ;;
  timing)
    # P0-2：计时 harness 模式——不依赖 msprof，跑工程自带/拷入的 aclrtEvent
    # harness 产 result.json（kernel_avg_us 的第一生产者，G4）。
    # harness 来源：工程自带 benchmark，或拷
    #   ops-family/references/common/timing_harness/timing_harness_aclrt_event.cpp
    # （模板拷贝纪律；[MODIFY] 点见模板头注释）。
    # 产物契约：harness 的 --output 必填（本脚本显式指定），结果落 $OUTPUT/ 并
    # manifest 记账；warmup/repeat 是口径字段由 harness 持有。EXE_ARGS 由调用方
    # 提供（含 --output 时以调用方为准；缺省追加本脚本的目标路径）。
    TIMING_JSON="$OUTPUT/result.json"
    TIMING_LOG="$OUTPUT/timing_harness.log"
    # 退出码先捕获再判定（跨 [[ ]] 取 $? 会拿到测试结果而非 harness 退出码——检视 #9 修复）
    timeout "$MSPROF_TIMEOUT" bash -c "cd '$PWD' && '${EXE}' ${EXE_ARGS} --output '$PWD/result.json'" >"$TIMING_LOG" 2>&1
    harness_rc=$?
    if [[ $harness_rc -eq 0 ]] && [[ -f "result.json" ]]; then
      mv "result.json" "$TIMING_JSON"
      record_block "timing_harness" ok "$TIMING_JSON" "harness 直跑（event 口径，warmup/repeat 见 result.json）"
    elif [[ -f "result.json" ]]; then
      mv "result.json" "$TIMING_JSON"
      record_block "timing_harness" failed "$TIMING_JSON" "harness 产出 result.json 但退出非 0（rc=$harness_rc），日志: $TIMING_LOG"
    else
      record_block "timing_harness" failed "$TIMING_LOG" "harness 退出非 0 且无 result.json（rc=$harness_rc），日志: $TIMING_LOG"
    fi
    ;;
  mc2)
    run_msprof_mc2
    ;;
  npu-compute)
    # ---- Roadmap #4：npu-compute DB 格式采集（950/A5 单卡）----
    # 产物形态：.npu-rep（单文件报告：7 Section CSV + Summary.jsonl +
    # summary.db[PcSampling/FieldUnitMap] + PipeTrace.json）→ 解包后喂
    # facts_bridge DB 通道 / verdict --db。
    # npu-compute 采集的指标范围以 --set full 为底座（7 Section 全量）；
    # PmSampling 等 Section 以 --sections 追加（本机 --list-sections 为门禁，
    # 先探测再选命令纪律同 msprof 段）。
    NPU_REP="$OUTPUT/report.npu-rep"
    NPU_LOG="$OUTPUT/npu_compute.log"
    # 可选 Section 追加（逗号分隔）：仅当本机支持时进命令（探测门禁）
    declare -a npc_cmd=(timeout "$MSPROF_TIMEOUT" npu-compute --set full)
    if [[ -n "${NPU_SECTIONS:-}" ]]; then
      IFS=',' read -r -a EXTRA_SECTIONS <<< "$NPU_SECTIONS"
      for s in "${EXTRA_SECTIONS[@]}"; do
        s="${s//[[:space:]]/}"
        [[ -z "$s" ]] && continue
        if npu-compute --list-sections 2>/dev/null | grep -qx "$s"; then
          npc_cmd+=(--section "$s")
        else
          echo "[warn] 本机 npu-compute 不支持 Section $s（--list-sections 为门禁），跳过" >&2
        fi
      done
    fi
    npc_cmd+=(--export "$NPU_REP" -- "$EXE")
    # EXE_ARGS 追加（bash -c 拼接与 msprof --application 形态对齐）
    if [[ -n "$EXE_ARGS" ]]; then
      npc_cmd+=($EXE_ARGS)
    fi
    if "${npc_cmd[@]}" >"$NPU_LOG" 2>&1 && [[ -f "$NPU_REP" ]]; then
      # 解包（--import；解包目录以终端 unpacked= 输出为准，不猜测）
      UNPACK_LOG="$OUTPUT/npu_compute_import.log"
      UNPACKED_DIR=""
      if timeout "$MSPROF_TIMEOUT" npu-compute --import "$NPU_REP" \
           --export "$OUTPUT/unpacked" >"$UNPACK_LOG" 2>&1; then
        UNPACKED_DIR=$(sed -n 's/.*npu-compute: unpacked= *//p' "$UNPACK_LOG" | tail -1)
      fi
      # 产物断言（P2-1 伪 Green 防护同款）：解包目录须含判定引擎主输入
      # （PipeUtilization.csv 或 summary.db 任一）
      ASSERT_NOTE=""
      if [[ -z "$UNPACKED_DIR" || ! -d "$UNPACKED_DIR" ]]; then
        ASSERT_NOTE="解包目录缺失或未解析到 unpacked= 路径（日志: $UNPACK_LOG）"
      elif [[ ! -f "$UNPACKED_DIR/PipeUtilization.csv" && ! -f "$UNPACKED_DIR/summary.db" ]]; then
        ASSERT_NOTE="解包目录无 PipeUtilization.csv / summary.db（判定引擎主输入）"
      fi
      DB_NOTE="无 summary.db"
      [[ -f "$UNPACKED_DIR/summary.db" ]] && DB_NOTE="含 summary.db（DB 通道可用）"
      if [[ -z "$ASSERT_NOTE" ]]; then
        record_block "npu-compute" ok "$NPU_REP" "unpacked=$UNPACKED_DIR（$DB_NOTE）"
      else
        record_block "npu-compute" partial "$NPU_REP" "断言未过：$ASSERT_NOTE（伪 Green 防护，P2-1）"
      fi
    else
      rc=$?
      if [[ $rc -eq 124 ]]; then
        record_block "npu-compute" failed "$NPU_REP" "npu-compute 超时（>${MSPROF_TIMEOUT}s），日志: $NPU_LOG"
      else
        record_block "npu-compute" failed "$NPU_REP" "npu-compute 退出非 0（rc=$rc），日志: $NPU_LOG"
      fi
    fi
    ;;
  *) echo "ERROR: 未知 mode: $MODE（standard/quick/timing/mc2/npu-compute）" >&2; exit 1 ;;
esac

# ---- manifest 落盘（采集块唯一真源；verdict.py 与报告按此定位产物）----
# F23：记录由 json.dumps 逐条生成（含引号/逗号安全），以 \x1f 分隔传递
BLOCKS_JSON=$(printf '\x1f%s' "${BLOCKS[@]:-}")
BLOCKS_JSON=${BLOCKS_JSON:1}
python3 - "$MANIFEST" "$MODE" "$EXE" "$EXE_ARGS" "$DEVICE" "$RANKS" "$KERNEL_NAME" "$BLOCKS_JSON" <<'PYEOF'
import json, sys
manifest = {
    "schema": "opstune.collection/1.0",
    "generated_at": __import__("datetime").datetime.now().isoformat(timespec="seconds"),
    "mode": sys.argv[2],
    "exe": sys.argv[3],
    "args": sys.argv[4],
    "device_id": sys.argv[5],
    "ranks": int(sys.argv[6]) if sys.argv[6].isdigit() else sys.argv[6],
    "kernel_name": sys.argv[7] or None,
    "blocks": [json.loads(b) for b in sys.argv[8].split("\x1f")] if sys.argv[8] else [],
}
with open(sys.argv[1], "w", encoding="utf-8") as f:
    json.dump(manifest, f, ensure_ascii=False, indent=2)
    f.write("\n")
PYEOF

echo "[done] manifest → $MANIFEST"
echo "下一步："
echo "  1. 在 op_summary/PipeUtilization CSV 上运行 ops-verdict/scripts/verdict.py"
echo "  2. 基线落盘：ops-verdict/scripts/snapshot.py（口径指纹）"
