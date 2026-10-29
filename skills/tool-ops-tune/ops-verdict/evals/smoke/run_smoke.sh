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
# run_smoke.sh — 离线干跑冒烟（检视 §7b / 整改批次 0）
#
# 目的：F1 类「bash -n 查不出、只有真跑才暴露」的 bug 在无 NPU 环境即暴露。
# 手段：msprof shim（按参数产出预置 CSV 目录的假可执行文件）+ 三模式
# collect_board.sh 干跑 + bash 语法检查 + isolation from-prof 干跑 +
# 规则包加载校验 + 族表资产断言。
#
# 用法：bash evals/smoke/run_smoke.sh   （在仓库根执行）
# 退出码：0 全绿 / 1 有失败（CI 门禁语义，与 evals/run_evals.py 同级）。

set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"  # skills/tool-ops-tune/
FAILURES=0

pass() { echo "[PASS] $1"; }
fail() { echo "[FAIL] $1"; FAILURES=$((FAILURES + 1)); }
check() { if [[ $? -eq 0 ]]; then pass "$1"; else fail "$1"; fi }

echo "=== smoke 1: bash 语法检查（collect_board.sh）==="
bash -n "$REPO_ROOT/ops-collect/scripts/collect_board.sh" 2>/dev/null
check "bash -n collect_board.sh"

echo "=== smoke 2: msprof shim 三模式干跑 ==="
SMOKE_TMP="$(mktemp -d)"
trap 'rm -rf "$SMOKE_TMP"' EXIT
SHIM_BIN="$SMOKE_TMP/bin"; mkdir -p "$SHIM_BIN"
SHIM_OUT="$SMOKE_TMP/shim_out"; mkdir -p "$SHIM_OUT"

# shim：解析 --output= 参数，产出预置 op_summary CSV（Task Duration + pipe 占比）
cat > "$SHIM_BIN/msprof" <<'SHIM'
#!/usr/bin/env bash
OUT=""
for a in "$@"; do
  case "$a" in --output=*) OUT="${a#--output=}" ;; esac
done
mkdir -p "$OUT/mindstudio_profiler_output"
cat > "$OUT/mindstudio_profiler_output/op_summary_1.csv" <<'CSV'
Task Duration(us),aic_time(us),aiv_time(us),aiv_vec_ratio,aic_cube_ratio,aic_mte2_ratio,aic_mte1_ratio,aic_fixpipe_ratio,ai_scalar_ratio,aic_mte3_ratio,ai_icache_miss_rate
100,0,100.5,0.32,0.00,0.55,0.02,0.01,0.22,0.03,0.9
100,0,99.8,0.31,0.00,0.56,0.02,0.01,0.21,0.03,0.9
CSV
SHIM
chmod +x "$SHIM_BIN/msprof"

for MODE in standard quick mc2; do
  OUT_DIR="$SMOKE_TMP/prof_$MODE"
  if PATH="$SHIM_BIN:$PATH" bash "$REPO_ROOT/ops-collect/scripts/collect_board.sh" \
      --exe /bin/true --mode "$MODE" --output "$OUT_DIR" >/dev/null 2>&1 \
      && python3 -c "
import json, sys
m = json.load(open('$OUT_DIR/collection_manifest.json'))
assert m['mode'] == '$MODE'
assert m['blocks'] and all(b['status'] == 'ok' for b in m['blocks']), m['blocks']
" 2>/dev/null; then
    pass "collect_board.sh --mode $MODE（shim 干跑 + manifest 全 ok）"
  else
    fail "collect_board.sh --mode $MODE"
  fi
done

echo "=== smoke 3: isolation from-prof 干跑（R 值档位从规则 YAML 派生）==="
if [ ! -f "$REPO_ROOT/ops-collect/scripts/isolation_test.py" ]; then
  echo "[SKIP] isolation_test.py 未交付（随多卡域 PR），smoke 3 暂跳过——交付后本段自动生效"
else
ISO_TMP="$SMOKE_TMP/iso"
mkdir -p "$ISO_TMP/comm/PROF_x/mindstudio_profiler_output" "$ISO_TMP/compute/PROF_y/mindstudio_profiler_output"
printf 'Task Duration(us),aic_mte2_ratio\n101,4\n102,4\n103,5\n104,5\n105,5\n106,5\n107,5\n108,5\n109,5\n110,5\n' \
  > "$ISO_TMP/comm/PROF_x/mindstudio_profiler_output/op_summary_a.csv"
printf 'Task Duration(us),aic_mte2_ratio\n251,40\n252,40\n253,41\n254,41\n255,41\n256,41\n257,41\n258,41\n259,41\n260,41\n' \
  > "$ISO_TMP/compute/PROF_y/mindstudio_profiler_output/op_summary_b.csv"
if python3 "$REPO_ROOT/ops-collect/scripts/isolation_test.py" \
    --exe /bin/true --switch-mode from-prof \
    --comm-prof "$ISO_TMP/comm" --compute-prof "$ISO_TMP/compute" --warmup 5 \
    --output "$SMOKE_TMP/iso_result.json" >/dev/null 2>&1 \
    && python3 -c "
import json
d = json.load(open('$SMOKE_TMP/iso_result.json'))
assert d['bound_hint'] == 'L3_MC2_STRONG_COMPUTE', d['bound_hint']
assert d['mte2_ratio_pure'] is not None
assert 'mte2_ratio_pure' in d['verdict_scalar']
" 2>/dev/null; then
  pass "isolation_test from-prof（R≈2.4 → STRONG_COMPUTE + mte2_ratio_pure 产出）"
else
  fail "isolation_test from-prof"
fi
fi

echo "=== smoke 4: 规则包加载校验（四包零误报）==="
if python3 "$REPO_ROOT/ops-verdict/scripts/verdict.py" \
    --rules "$REPO_ROOT/ops-verdict/rules/L3-inter-card.yaml,$REPO_ROOT/ops-verdict/rules/L2-inter-core.yaml,$REPO_ROOT/ops-verdict/rules/L1-single-core.yaml,$REPO_ROOT/ops-verdict/rules/L0-instruction.yaml" \
    --soc a5 --scalar t_comm_us=100,t_compute_us=250 \
    -o "$SMOKE_TMP/v.json" >/dev/null 2>&1; then
  pass "四规则包全瀑布加载校验通过（V1-V4 零误报）"
else
  fail "四规则包全瀑布加载校验"
fi

echo "=== smoke 5: 防回归自证——顶层 local 注入必须被冒烟抓住 ==="
INJECT_TMP="$SMOKE_TMP/inject.sh"
cp "$REPO_ROOT/ops-collect/scripts/collect_board.sh" "$INJECT_TMP"
# 在 mc2 case 前注入一行顶层 local（复刻 F1 bug 形态）
python3 - "$INJECT_TMP" <<'PYEOF'
import sys
from pathlib import Path
p = Path(sys.argv[1])
t = p.read_text(encoding="utf-8")
t = t.replace("  mc2)\n    run_msprof_mc2\n", "  mc2)\n    local injected=1\n    run_msprof_mc2\n")
p.write_text(t, encoding="utf-8")
PYEOF
if bash "$INJECT_TMP" --exe /bin/true --mode mc2 --output "$SMOKE_TMP/prof_inject" >/dev/null 2>&1; then
  fail "顶层 local 注入未被抓住（冒烟网失效）"
else
  pass "顶层 local 注入 → mc2 模式退出非 0（F1 类 bug 冒烟可抓）"
fi

echo "=== smoke 6: 族表资产静态断言（防 SKILL.md 数据漂移）==="
if [ -f "$REPO_ROOT/ops-family/scripts/family_asset_audit.py" ]; then
  python3 "$REPO_ROOT/ops-family/scripts/family_asset_audit.py" >/dev/null 2>&1
  check "family_asset_audit.py（族表模板数与实测一致）"
else
  echo "[SKIP] family_asset_audit.py 未交付（随算子族 PR），smoke 6 暂跳过——交付后本段自动生效"
fi

echo "=== smoke 7: facts_bridge 真实导出干跑（G2 标量生产者）==="
BRIDGE_TMP="$SMOKE_TMP/bridge"
mkdir -p "$BRIDGE_TMP/fixtures"
# 用 fixture 形态（本仓 6 个 fixtures 同款逐核明细 + 顶层 7 组导出）构造最小产物集
printf 'block_id,sub_block_id,aic_time(us),aiv_time(us),aiv_vec_ratio,aic_cube_ratio,aic_mte2_ratio,aic_mte1_ratio,aic_fixpipe_ratio,ai_scalar_ratio,aic_mte3_ratio,ai_icache_miss_rate,aiv_mte2_active_bw(GB/s)\n0,Vec0,0,100.5,0.32,0.00,0.55,0.02,0.01,0.22,0.03,1.2,1200.0\n1,Vec0,0,99.8,0.31,0.00,0.56,0.02,0.01,0.21,0.03,1.1,1240.0\n' > "$BRIDGE_TMP/fixtures/PipeUtilization.csv"
printf 'block_id,sub_block_id,aiv_total_hit_rate(%%)\n0,Vec0,96.1\n1,Vec0,95.7\n' > "$BRIDGE_TMP/fixtures/L2Cache.csv"
printf 'block_id,sub_block_id,aiv_vec_fp32_ratio\n0,Vec0,0.61\n1,Vec0,0.63\n' > "$BRIDGE_TMP/fixtures/ArithmeticUtilization.csv"
printf 'block_id,sub_block_id,aiv_vec_total_cflt_ratio\n0,Vec0,0.08\n1,Vec0,0.07\n' > "$BRIDGE_TMP/fixtures/ResourceConflictRatio.csv"
printf 'Op Name,Op Type,Task Duration(us),Block Dim\nop866214,vector,14.7,8\n' > "$BRIDGE_TMP/fixtures/OpBasicInfo.csv"
if python3 "$REPO_ROOT/ops-collect/scripts/facts_bridge.py" \
    --manifest "$BRIDGE_TMP/fixtures" --soc a2 --output "$BRIDGE_TMP/scalars.json" >/dev/null 2>&1 \
    && python3 -c "
import json, sys
d = json.load(open('$BRIDGE_TMP/scalars.json'))
s = d['scalars']
assert abs(s['l2_hit_rate'] - 95.9) < 0.01, s
assert abs(s['icache_miss_rate'] - 1.15) < 0.01, s
assert abs(s['vec_fp32_ratio'] - 62.0) < 0.01, s          # 0~1 比例 ×100
assert abs(s['vec_total_cflt_ratio'] - 8.0) < 0.01, s      # 0~1 比例 ×100（mean 0.075 → round 0.08 ×100）
assert abs(s['mte2_active_bw_pct_of_peak'] - 76.25) < 0.01, s  # 1220/1600 GB/s
assert abs(s['task_duration_us'] - 14.7) < 0.01, s
assert s['block_dim'] == 8 and s['physical_cores'] == 24, s
assert d['missing'] == [], d['missing']
" 2>/dev/null; then
  pass "facts_bridge 标量提取与峰值换算（7 标量 + 换算正确）"
else
  fail "facts_bridge 标量提取"
fi



echo "=== smoke 8: timing 模式干跑（P0-2 计时 harness 归档）==="
TIMING_TMP="$SMOKE_TMP/timing"
mkdir -p "$TIMING_TMP"
cat > "$TIMING_TMP/fake_harness.sh" <<'HARNESS'
#!/usr/bin/env bash
echo '{"schema":"opstune.timing/1.0","kernel_avg_us":14.2,"warmup":5,"repeat":20}' > result.json
HARNESS
chmod +x "$TIMING_TMP/fake_harness.sh"
( cd "$TIMING_TMP" && PATH="$SHIM_BIN:$PATH" bash "$REPO_ROOT/ops-collect/scripts/collect_board.sh" \
    --exe "$TIMING_TMP/fake_harness.sh" --mode timing --output "$TIMING_TMP/prof" ) >/dev/null 2>&1
if python3 -c "
import json
m = json.load(open('$TIMING_TMP/prof/collection_manifest.json'))
assert m['mode'] == 'timing', m['mode']
assert m['blocks'][0]['name'] == 'timing_harness' and m['blocks'][0]['status'] == 'ok', m['blocks']
r = json.load(open('$TIMING_TMP/prof/result.json'))
assert r['kernel_avg_us'] == 14.2
" 2>/dev/null; then
  pass "collect_board.sh --mode timing（harness result.json 归档 + manifest ok）"
else
  fail "collect_board.sh --mode timing"
fi


echo "=== smoke 9: tiling_candidates 四族干跑（候选生成 + 容量校验）==="
if [ ! -f "$REPO_ROOT/ops-family/scripts/tiling_candidates.py" ]; then
  echo "[SKIP] tiling_candidates.py 未交付（随算子族 PR），smoke 9 暂跳过——交付后本段自动生效"
else
TILING_TMP="$SMOKE_TMP/tiling"; mkdir -p "$TILING_TMP"
if python3 "$REPO_ROOT/ops-family/scripts/tiling_candidates.py" \
    --family elementwise --shape 8448,4096 --dtype fp16 --soc a5 \
    --output "$TILING_TMP/cand.json" >/dev/null 2>&1 \
    && python3 "$REPO_ROOT/ops-family/scripts/tiling_candidates.py" \
    --family reduction --shape 32,4096 --dtype fp16 --soc a5 --axis -1 \
    --output "$TILING_TMP/red.json" >/dev/null 2>&1 \
    && python3 -c "
import json
c = json.load(open('$TILING_TMP/cand.json'))
assert c['soc_caps']['core_num'] == 28 and c['soc_caps']['ub_kb'] == 248, c['soc_caps']
fc = next(x for x in c['candidates'] if x['variant'] == 'full-cores')
assert fc['block_dim'] == 28 and fc['aligned'], fc
assert fc['block_former'] % 512 == 0, fc        # 多核 512 元素对齐
assert fc['capacity_kb'] <= 248, fc             # 容量预算不超限
r = json.load(open('$TILING_TMP/red.json'))
assert r['pruned'] and r['pruned'][0]['variant'] == 'full-load', r  # 整行驻留不可行被剪枝
assert r['candidates'][0]['block_dim'] == 28, r
" 2>/dev/null; then
  pass "tiling_candidates（elementwise 满核对齐 + reduction full-load 剪枝记录）"
else
  fail "tiling_candidates 四族干跑"
fi

fi


echo "=== smoke 10: collect_cases → batch 直连端到端（P0-1 A1 铁律一条命令）==="
P1_TMP="$SMOKE_TMP/p1"; mkdir -p "$P1_TMP/bin"
cat > "$P1_TMP/cases.csv" <<'CASES'
case_id,shape,dtype,ref_impl
c01,8448x4096,fp16,aclnnGelu
c02,4096x4096,fp16,aclnnGelu
c03,1024x1024,bf16,aclnnGelu
CASES
cat > "$P1_TMP/bin/msprof" <<'P1SHIM'
#!/usr/bin/env bash
OUT=""
for a in "$@"; do
  case "$a" in --output=*) OUT="${a#--output=}" ;; esac
done
mkdir -p "$OUT/mindstudio_profiler_output"
printf 'Task Duration(us),aiv_vec_ratio,aic_mte2_ratio\n100.5,0.32,0.55\n101.2,0.31,0.56\n' \
  > "$OUT/mindstudio_profiler_output/op_summary_1.csv"
P1SHIM
chmod +x "$P1_TMP/bin/msprof"
# scope.json（12 HARD 字段齐）
cat > "$P1_TMP/scope.json" <<'SCOPE'
{"schema":"opstune.scope/1.0","op":"gelu","soc":"a5","mode":"board","timing_method":"msprof_task_duration",
 "baseline_kind":"system","cann_version":"8.3.0","device_id":"0","shape":"8448x4096","dtype":"fp16",
 "format":"ND","warmup":5,"repeat":10}
SCOPE
( cd "$P1_TMP" && PATH="$P1_TMP/bin:$PATH" python3 \
    "$REPO_ROOT/ops-collect/scripts/collect_cases.py" \
    --cases "$P1_TMP/cases.csv" --exe /bin/true --mode quick \
    --output "$P1_TMP/collect_out" ) >/dev/null 2>&1
# 模拟标杆侧数据（system 行：80/82/40us）
printf 'c01,system,80.0,pass\nc02,system,82.0,pass\nc03,system,40.0,pass\n' \
  >> "$P1_TMP/collect_out/perf_data.csv"
if python3 "$REPO_ROOT/ops-verdict/scripts/batch_pipeline.py" \
    --from-collect "$P1_TMP/collect_out" --cases "$P1_TMP/cases.csv" \
    --baseline-variant system --after-variants custom \
    --scope "$P1_TMP/scope.json" --output-dir "$P1_TMP/matrix" >/dev/null 2>&1 \
    && python3 -c "
import csv, json
rows = list(csv.DictReader(open('$P1_TMP/collect_out/perf_data.csv')))
assert len(rows) == 6, rows                       # 3 case × 2 variant（custom 采集 + system 追加）
assert {(r['case_id'], r['variant']) for r in rows if r['variant']=='custom'} == \
       {('c01','custom'),('c02','custom'),('c03','custom')}
md = open('$P1_TMP/matrix/comparison_matrix.md').read()
assert 'geomean speedup' in md and 'c03' in md    # 矩阵产出且逐 case 覆盖（A1）
assert '0.793x' in md or 'REVERT' in md           # vs 标杆读数成立（100.85/80 → 劣化如实呈现）
s = json.load(open('$P1_TMP/collect_out/collect_summary.json'))
assert s['ok'] == 3 and s['failed_or_partial'] == [], s
" 2>/dev/null; then
  pass "collect_cases → batch --from-collect（cases.csv 到对比矩阵零手工中间产物）"
else
  fail "P0-1 直连端到端"
fi

echo "=== smoke 11: csv_to_db + facts_bridge DB 通道端到端（Roadmap #4）==="
DB_TMP="$SMOKE_TMP/db_channel"; mkdir -p "$DB_TMP/fixtures"
# fixture：npu-compute 解包形态（Section CSV 顶层平铺 + summary.jsonl）
printf 'block_id,sub_block_id,aic_time(us),aiv_time(us),aiv_vec_ratio,aiv_mte2_active_bw(GB/s)\n0,vector0,NA,100.5,0.32,1200.0\n1,vector0,NA,99.8,0.31,1240.0\n' > "$DB_TMP/fixtures/PipeUtilization.csv"
printf 'block_id,sub_block_id,aiv_read_hit_rate(%%)\n0,vector0,96.1\n1,vector0,95.7\n' > "$DB_TMP/fixtures/L2Cache.csv"
printf 'block_id,sub_block_id,aiv_vec_stu_cflt_ratio,aiv_vec_ldu_cflt_ratio,aiv_vec_sfu_cflt_ratio\n0,vector0,0.01,0.03,0.00\n1,vector0,0.01,0.05,0.00\n' > "$DB_TMP/fixtures/ResourceConflictRatio.csv"
printf 'block_id,sub_block_id,aiv_vec_vf_ratio\n0,vector0,0.61\n1,vector0,0.63\n' > "$DB_TMP/fixtures/ArithmeticUtilization.csv"
printf '{"category":"OpInfoSummary","Op Name":"vec_demo","Op Type":"vector","Task Duration(us)":51.7,"Block Dim":8}\n' > "$DB_TMP/fixtures/summary.jsonl"
if python3 "$REPO_ROOT/ops-collect/scripts/csv_to_db.py" \
    --csv-dir "$DB_TMP/fixtures" --db "$DB_TMP/fixtures/summary.db" --quiet >/dev/null 2>&1 \
  && ! python3 "$REPO_ROOT/ops-collect/scripts/csv_to_db.py" \
    --csv-dir "$DB_TMP/fixtures" --db "$DB_TMP/fixtures/summary.db" --quiet >/dev/null 2>&1 \
  && python3 -c "
import sqlite3
# 注入 PcSampling 行（DB 独有表——真源 DDL 同构，冒烟只验证消费链不验证采集）
db = sqlite3.connect('$DB_TMP/fixtures/summary.db')
db.execute('''CREATE TABLE PcSampling(address INTEGER NOT NULL PRIMARY KEY,
    ibuf_empty INTEGER NOT NULL, nop_cycles INTEGER NOT NULL,
    scoreboard_not_ready INTEGER NOT NULL, register_bank_conflict INTEGER NOT NULL,
    resource_conflict INTEGER NOT NULL, warp_level_sync INTEGER NOT NULL,
    divergence_stack_spill INTEGER NOT NULL, others INTEGER NOT NULL)''')
db.execute('INSERT INTO PcSampling VALUES(616,0,210,16320,0,0,0,0,0)')
db.commit(); db.close()
" 2>/dev/null \
  && python3 "$REPO_ROOT/ops-collect/scripts/facts_bridge.py" \
    --manifest "$DB_TMP/fixtures" --soc a5 --no-db \
    --output "$DB_TMP/scalars_csv.json" >/dev/null 2>&1 \
  && python3 "$REPO_ROOT/ops-collect/scripts/facts_bridge.py" \
    --manifest "$DB_TMP/fixtures" --soc a5 \
    --output "$DB_TMP/scalars_db.json" >/dev/null 2>&1 \
  && python3 -c "
import json
d_csv = json.load(open('$DB_TMP/scalars_csv.json'))
d_db = json.load(open('$DB_TMP/scalars_db.json'))
assert d_csv['source_tool'] == 'npu-compute' and d_db['source_tool'] == 'npu-compute'
s_db = d_db['scalars']
# DB 通道产出（PcSampling/OpInfoSummary/列候选扩充三类新能力全断言）：
assert abs(s_db['top_stall_pct'] - 98.73) < 0.01, s_db     # PcSampling → L0 生产者
assert abs(s_db['task_duration_us'] - 51.7) < 0.01, s_db  # Summary.jsonl OpInfoSummary
assert abs(s_db['block_dim'] - 8.0) < 0.01, s_db
assert abs(s_db['l2_hit_rate'] - 95.9) < 0.01, s_db        # read_hit_rate 口径补位
assert abs(s_db['vec_total_cflt_ratio'] - 5.0) < 0.01, s_db  # stu+ldu+sfu 分量和 ×100
assert d_db['pcsampling'] and d_db['pcsampling']['top_stall'] == 'scoreboard_not_ready'
# 双通道等价（同数据两格式，标量必须一致）：
common = set(d_csv['scalars']) - {'top_stall_pct'}   # top_stall 为 DB 独有
assert common == set(d_db['scalars']) - {'top_stall_pct'}, (d_csv['scalars'], d_db['scalars'])
for k in common:
    assert abs(d_csv['scalars'][k] - d_db['scalars'][k]) < 0.02, (k, d_csv['scalars'][k], d_db['scalars'][k])
" 2>/dev/null; then
  pass "csv_to_db 转换（覆盖保护：二次转换拒覆盖）+ facts_bridge DB 通道（top_stall/OpInfoSummary/双通道等价）"
else
  fail "facts_bridge DB 通道端到端"
fi

echo "=== smoke 12: db_bench 等价性门禁（W1-W3 双通道数值一致）==="
if python3 "$REPO_ROOT/ops-collect/scripts/db_bench.py" \
    --dir "$DB_TMP/fixtures" --repeat 3 \
    -o "$DB_TMP/db_bench_report.json" >/dev/null 2>&1 \
  && python3 -c "
import json
r = json.load(open('$DB_TMP/db_bench_report.json'))
assert r['schema'] == 'opstune.db-bench/1.0'
assert r['all_equivalent'] is True, r
for w in ('W1-facts-scalar-extract', 'W2-verdict-full-load', 'W3-analytical-batch'):
    assert r['workloads'][w]['equivalent'] is True
    assert r['workloads'][w]['csv_ms'] > 0 and r['workloads'][w]['db_ms'] > 0
# 不断言具体倍数（机器相关）；W4 PcSampling 独有断言：
assert r['workloads']['W4-pcsampling-dist']['db_value'] is not None
" 2>/dev/null; then
  pass "db_bench 等价性门禁（提速比如实呈现不作门禁）"
else
  fail "db_bench 等价性"
fi

echo "=== smoke 13: collect_board --mode npu-compute shim 干跑（DB 格式采集编排）==="
NPC_TMP="$SMOKE_TMP/npc"; mkdir -p "$NPC_TMP/bin" "$NPC_TMP/fixtures/unpacked"
# fixture：解包形态目录（采集 shim 的 --import 复制源）
printf 'block_id,sub_block_id,aic_time(us),aiv_time(us),aiv_vec_ratio\n0,vector0,NA,100.5,0.32\n' > "$NPC_TMP/fixtures/unpacked/PipeUtilization.csv"
cp "$NPC_TMP/fixtures/unpacked/PipeUtilization.csv" "$NPC_TMP/fixtures/unpacked/L2Cache.csv"
# shim npu-compute：--list-sets/--list-sections 查询门禁 + --export 落 rep 副本 +
# --import 复制 fixture 目录并打印 unpacked= 路径（真实工具的终端契约）
cat > "$NPC_TMP/bin/npu-compute" <<'NPCSHIM'
#!/usr/bin/env bash
FIXTURE="$(dirname "$0")/../fixtures"
IMPORT=""; EXPORT=""
for ((i=1; i<=$#; i++)); do
  arg="${!i}"
  case "$arg" in
    --list-sets) echo "basic: Pipeline PipeUtilization Memory MemoryL0 MemoryUB L2Cache ArithmeticUtilization"; echo "full: + ResourceConflictRatio"; exit 0 ;;
    --list-sections) printf 'PipeUtilization\nMemory\nMemoryL0\nMemoryUB\nL2Cache\nPipeline\nArithmeticUtilization\nResourceConflictRatio\n'; exit 0 ;;
    --import) n=$((i+1)); IMPORT="${!n}" ;;
    --export) n=$((i+1)); EXPORT="${!n}" ;;
  esac
done
if [[ -n "$IMPORT" ]]; then
  DIR="$EXPORT/unpacked_1"; mkdir -p "$DIR"
  cp -r "$FIXTURE/unpacked/." "$DIR/"
  echo "npu-compute: unpacked= $DIR"; exit 0
fi
printf 'npu-rep-container-fixture' > "$EXPORT"
echo "npu-compute: report= $EXPORT"; exit 0
NPCSHIM
chmod +x "$NPC_TMP/bin/npu-compute"
if PATH="$NPC_TMP/bin:$PATH" bash "$REPO_ROOT/ops-collect/scripts/collect_board.sh" \
    --exe /bin/true --mode npu-compute --sections PmSampling,BogusSection \
    --output "$NPC_TMP/npc_out" >/dev/null 2>&1 \
  && python3 -c "
import json
m = json.load(open('$NPC_TMP/npc_out/collection_manifest.json'))
assert m['mode'] == 'npu-compute'
b = m['blocks'][0]
assert b['name'] == 'npu-compute' and b['status'] == 'ok', m
assert 'unpacked=' in b['note'], b
# 产物断言（伪 Green 防护路径）：解包目录主输入在位
# （note 含中文括号注记——用 (\S+) 在 ASCII 路径后截断）
import pathlib, re
unpacked = re.search(r'unpacked=(\S+)', b['note']).group(1)
unpacked = unpacked.rstrip('（无含')
assert (pathlib.Path(unpacked) / 'PipeUtilization.csv').exists(), unpacked
" 2>/dev/null; then
  pass "collect_board --mode npu-compute（探测门禁 + manifest + 解包产物断言）"
else
  fail "npu-compute 采集模式干跑"
fi

echo ""
if [[ $FAILURES -eq 0 ]]; then
  echo "smoke: 全部通过"
  exit 0
else
  echo "smoke: $FAILURES 项失败 —— F1 类运行时 bug 防线失效，禁止合入"
  exit 1
fi
