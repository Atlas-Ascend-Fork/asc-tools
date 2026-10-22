# Subagent 调用参数详情（OpsTuningAgent 唯一执行手册）

> 每个 Step：调用参数、关联 SKILL、完成验证、约束提醒。占位符：`{code_dir}`
> `{cases_csv}` `{output_dir}` `{operator}`。

---

## Step 0：意图与边界（Phase 0）

主 agent 亲自执行（不派 subagent——精度门禁预检必须由编排层亲自把关）。

### 1. 精度门禁预检

对用户提供的测试用例**全量**跑一遍工程现有正确性/精度测试（禁止抽样）：

```bash
# 工程自带测试入口（示例形态，以工程 README/run 脚本为准）：
cd {code_dir} && bash run.sh            # 或 ./build/ut --gtest_filter=... 或 python3 verify.py
# 记录：总 case 数、PASS 数、FAIL 数、容差口径（工程现有容差，禁止自创）
```

- 任一 case FAIL / 崩溃 / 挂起 → **拒绝进入调优**，转调试域（回报用户：
  「精度未通过（N/M FAIL），先处置正确性再谈性能」）
- 无法运行（缺依赖/构建失败）→ 同样拒绝，转工程问题处置

### 2. 场景分类判据

| 判据 | 场景 | 裁剪 |
|------|------|------|
| 源码含 fork 多进程绑卡 / SHMEM UDMA / BarrierAll / CrossCoreFlag / HCCL 通信 | 融合算子（MC²） | Phase 2 加污染前置 + L3 全启用 + 隔离测试 |
| 多算子融合单 kernel（FA 类） | 融合算子（FA） | 同上（fa 族走无同型降级策略） |
| 用户给 profiling 产物目录、无源码 | 问答模式 | 仅 Phase 2 离线判定 + 报告解读 |
| 用户要求批量回归（已有基线库） | 批量回归 | Phase 1 简化 + Phase 3-4 改重跑判定+对比 |
| 其余 | 单算子 | 全五阶段 |

### 3. 生成 scope.json（opstune.scope/1.0）

字段与 delta_compare HARD_KEYS **严格同名同义**（口径互锁的上游真源；
schema 与最小示例见 `ops-verdict/references/scope-schema.md`（相对本套件根目录））：

```bash
python3 - <<'PYEOF'
import json, datetime
scope = {
  "schema": "opstune.scope/1.0",
  "operator": "{operator}",           # 算子名
  # ---- HARD_KEYS 12 字段（与 snapshot/delta_compare 同名同义）----
  "op": "{operator}", "soc": "<a2|a3|a5>", "mode": "board",
  "timing_method": "<event|sim|msprof|msprof_task_duration|profiler_summary>",
  "baseline_kind": "<system|self_before_after>", "cann_version": "<ver>",
  "device_id": "<id>", "shape": "<代表 case>", "dtype": "<dt>",
  "format": "<ND|NZ|...>", "warmup": <n>, "repeat": <n>,
  # ---- 机制变量 2 字段 ----
  "tiling_key": null, "block_dim": null,
  # ---- 场景与目标 ----
  "scene": "<single|mc2|fa|regression|qa>",
  "cases_csv": "{cases_csv}", "code_dir": "{code_dir}",
  "performance_goal": "<目标或 null>", "max_rounds": <n>,
  "precision_gate": {"total": <N>, "pass": <N>, "fail": <N>, "tolerance": "<工程现有容差>"},
  "generated_at": datetime.datetime.now().isoformat(timespec="seconds"),
}
json.dump(scope, open("{output_dir}/scope.json", "w"), ensure_ascii=False, indent=2)
PYEOF
```

### 完成验证

- scope.json 存在且 12 HARD 字段齐全（`python3 -c "import json; s=json.load(open('{output_dir}/scope.json')); assert all(k in s for k in ('op','soc','mode','timing_method','baseline_kind','cann_version','device_id','shape','dtype','format','warmup','repeat'))"`)
- precision_gate.fail == 0（否则 Phase 0 未过，禁止进入 Step 1）

### 约束提醒

- 精度门禁预检禁止只跑代表性用例（全量铁律从 Phase 0 开始生效）
- scope.json 的 HARD 字段后续任何变化都必须重建基线（与 A4 铁律同源）

---

## Step 1：采集与基线

### 调用方式

本阶段由主 agent 亲自执行（调用 SKILL，不派 subagent——基线是不可变口径，须一次建对）。

1. 加载 `/ops-collect`，按其路由决策树执行：
   - MC² / fork 多 rank → `collect_board.sh --mode mc2`（+ 后续 Step 2 的隔离测试）
   - 单卡 → `--mode standard`（7 组 aic-metrics）
2. 环境指纹存档 `tuning_artifacts/{operator}/environment.txt`（npu-smi / CANN 版本 /
   设备 ID / 频率 / 同卡负载 / repo commit）
3. **不可变基线**落盘：

```bash
python3 <ops-verdict>/scripts/snapshot.py \
  --op {operator} --variant baseline --mode board --soc <soc> \
  --precision pass --kernel-avg-us <值> --timing-method <五选一> \
  --baseline-kind <system|self_before_after> --cann-version <ver> \
  --device-id <id> --shape <代表case> --dtype <dt> --format <fmt> \
  --warmup <n> --repeat <n> \
  -o {output_dir}/baseline/result.json
```

计时口径选择（短 kernel 多 launch 单 event / 长 kernel 允许 Task Duration 注明口径 /
冷稳态固定 warmup / MC² 循环 ≥10 前 5 warm-up 跨 rank max）详见 /ops-collect SKILL.md。

### 完成验证

- `{output_dir}/baseline/result.json` 存在且 `precision=pass`
- `collection_manifest.json` 无 failed 块（failed → 本 Step 未过）
- scope.json 12 HARD 字段齐全（见 Step 0 / scope-schema.md）

### 约束提醒

- 全量 case 覆盖：>20 取前 20 并告知用户截断
- 禁 host 侧计时替代 msprof（MC²）；禁 -O0 构建

---

## Step 2：分层判定

### 调用方式

主 agent 执行（判定出自脚本，主 agent 只组装输入与解读输出）：

```bash
# 单算子：
python3 <ops-verdict>/scripts/verdict.py \
  --rules <ops-verdict>/rules/L2-inter-core.yaml,<ops-verdict>/rules/L1-single-core.yaml[,<ops-verdict>/rules/L0-instruction.yaml] \
  --csv <PipeUtilization.csv> --soc <soc> \
  --scalar physical_cores=<n>,block_dim=<n>,task_duration_us=<n>[,mte2_active_bw_pct_of_peak=<n>] \
  -o {output_dir}/baseline/verdict.json

# MC²（先隔离测试再全瀑布）：
python3 <ops-collect>/scripts/isolation_test.py --exe <exe> --args "..." \
  --output {output_dir}/iso_result.json
python3 <ops-verdict>/scripts/verdict.py \
  --rules <ops-verdict>/rules/L3-inter-card.yaml,<ops-verdict>/rules/L2-inter-core.yaml,<ops-verdict>/rules/L1-single-core.yaml \
  --csv <PipeUtilization.csv> --soc a5 \
  --scalar t_comm_us=<iso>,t_compute_us=<iso>[,mte2_ratio_full=<n>,mte2_ratio_pure=<n>,physical_cores=<n>] \
  -o {output_dir}/baseline/verdict.json
```

### 完成验证

- `verdict.json` 存在；`verdict` ∈ {NEEDS_OPTIMIZATION, NEAR_LIMIT, NO_BOUND_FOUND}
- INSUFFICIENT_DATA → 按 `skipped[].missing` 补采（回 Step 1 采集，≤2 轮）；仍缺 → 如实上报，禁止猜测式判定
- NO_BOUND_FOUND → 未命中任何瓶颈规则：评估是否已达目标；仍需提升则采集 L0 源码级指标做深度分析

### 约束提醒

- 主 agent **不得**在 verdict 之外补充「自己观察到的 bound」——只能作为假设标注缺失指标
- NEAR_LIMIT：向用户报告「接近上限」读数（含 speedup 上界依据），确认后再决定是否继续

---

## Step 3：候选方案

### 调用方式

主 agent 执行（渐进式披露：只加载 findings 命中的机制卡）：

1. 加载 `/ops-mechanism`：按 `findings[].mechanism_refs` 逐张打开机制卡；同型案例最多 3 个
   （A-D/E 分级；无同型走降级策略并在方案卡标注）
2. 加载 `/ops-family`：方案涉及模板 → 查找链路取模板路径，标注可用性分类
   （✅可直接拷贝 / ⚠️部分实现 / ❌仅设计参考）
3. 输出 `{output_dir}/candidates.json`（≤3 张方案卡，按 speedup_est × 可行性排序，
   每卡含 mechanism/target_rule/evidence/diff_sketch/speedup_est/semantic_risk/
   precision_plan/verify_cmd/rollback_if/template）

### 完成验证

- 每张卡 target_rule ∈ verdict findings 的 rule_id（证据绑定）
- 每张卡 rollback_if 非空；semantic_risk=altered 的卡 precision_plan 含边界用例

### 约束提醒

- 候选必须有当前算子的 profiling 证据（案例数字外推禁止）
- 融合算子注意机制互斥（如 MTE2 污染下禁单核访存机制——verdict 路由副作用已处理）

---

## Step 4：单轮实施

### 阶段 4a：并行分发（主 agent → perf-implementer × N）

每张方案卡一个 subagent（并行）：

```
{
  "description": "实施机制：<mechanism>",
  "subagent_type": "perf-implementer",
  "prompt": "
请实施以下单个优化机制（单变量铁律：只做这一项）：

- 算子源码目录：{code_dir}
- 方案卡：<该方案的完整 candidates.json 条目，含 template 路径与 rollback_if>
- 测试用例文件：{cases_csv}
- 输出目录：{output_dir}/{operator}_opt_<mechanism>/

按 perf-implementer.md 执行流程完成：复制目录 → 模板拷贝优先（.template 先转换）→
MicroAPI 编译修复顺序（禁直接降级）→ 编译 → 全量 case 精度验证（工程现有容差）。
返回：代码目录、编译状态、逐 case PASS/FAIL 明细、模板使用情况、diff 摘要。
不采集性能、不生成对比报告。
  "
}
```

### 阶段 4b：实施审查（主 agent → perf-reviewer，机器闸门先行）

perf-implementer 返回后、统一采集**之前**，每机制两道审查：

1. **diff_audit（机器闸门，主 agent 亲自跑）**：

```bash
python3 scripts/diff_audit.py \
  --baseline {code_dir} --opt {output_dir}/{operator}_opt_<mechanism>/ \
  --candidates {output_dir}/candidates.json --index <该方案卡序号> \
  -o {output_dir}/round-<N>-<mechanism>/diff_audit.json
```

   - exit 2（UNATTRIBUTABLE：改动超出方案卡 diff_sketch_files 声明范围）
     → 该轮**不进统一采集**（采集了也无法归因），处置二选一：回滚超范围
     改动重测 / implementer 补充原子性论证并把文件补进方案卡后重审；
   - 方案卡未声明 diff_sketch_files → 只记录不判罚（不能拿不存在的声明当红线），
     audit.json 留档供 perf-reviewer 逐文件判读。

2. **perf-reviewer（独立审查 subagent）**：

```
{
  "description": "审查实施：<mechanism>",
  "subagent_type": "perf-reviewer",
  "prompt": "
请独立审查以下优化副本（对象：只做方案卡声明的这一件事、且做对了）：

- 方案卡：<该方案的完整条目，含 diff_sketch_files / template / semantic_risk / precision_plan>
- 基线源码目录：{code_dir}
- 优化副本目录：{output_dir}/{operator}_opt_<mechanism>/
- diff_audit.json：<主 agent 产出的机器审查结果路径>
- 测试用例文件：{cases_csv}

按 perf-reviewer.md 执行流程：独立重编 → 单变量符合性判读（消费 diff_audit）→
模板保真（MicroAPI VF 降级检测）→ 精度复核（semantic_risk=altered 的边界用例）。
输出 {output_dir}/round-<N>-<mechanism>/review.md，结论四档：
PASS / PASS WITH NOTES / FAIL / UNATTRIBUTABLE。
不采集性能、不跑 delta_compare。
  "
}
```

   - FAIL / UNATTRIBUTABLE → 该轮按处置规则回到 implementer（修复或回滚），
     **性能数字不得进入任何报告结论**（与精度 fail 同档）；
   - 多 KEEP 仲裁**必须**在全部 KEEP 轮的 review.md 均 PASS / PASS WITH NOTES
     之后进行。

### 阶段 4c：统一采集与对比（主 agent 垄断）

1. 对基线 + 各机制目录，逐 case 同口径采集（与 Step 1 完全相同的 timing/warmup/repeat/device）。
   **工具化（P0-1）**：`collect_cases.py`（cases.csv + exe → 逐 case manifest + perf_data.csv）
   替代手工串接——case 间串行同卡、防重采闸门、failed 如实呈现；
   `batch_pipeline.py --from-collect` 直连产出对比矩阵，零手工中间产物
2. 每机制落 snapshot：`round-<N>-<mechanism>/result.json`
3. 逐机制对比：

```bash
python3 <ops-verdict>/scripts/delta_compare.py \
  --baseline {output_dir}/baseline/result.json \
  --after {output_dir}/round-<N>-<mechanism>/result.json \
  --output {output_dir}/round-<N>-<mechanism>/comparison.md
```

4. 三段结论处置：KEEP（保留为新基线）/ REVERT（回滚后复跑确认回基线水平）/ 噪声带内
   （repeat 翻倍复测：以新 repeat 同步重采基线与优化侧、新目录重建两侧快照后再比）
5. 机制生效证据复采：verdict.py 复跑（期望 target_rule 转 NOT_HIT 或 severity 降档）

### 多 KEEP 仲裁（并行候选都 KEEP 时的处置规则）

并行分发 ≤3 张方案卡（各自对照同一基线）后出现**多个 KEEP** 时：

1. **新基线选择**：按「speedup × 确定性」排序取最优者为新基线——
   speedup 取该机制 comparison 的实测值，确定性按 confidence（high > medium > low）；
   并列时取 speedup_range 下界更高者（保守估计更强）。
2. **其余 KEEP 产物降级**：不并入新基线，降级为下一轮候选优先项
   （其 diff 保留在各自 round 目录，下一轮以新基线重新实施验证——
   与「每轮必须重新走 Phase 2」的多轮机制一致）。
3. **同代码区域冲突检测**：多张方案卡的 diff_sketch 涉及同一文件/同一代码区域时
   **禁止并行、改串行**（互踩无法归因）；主 agent 在 4a 分发前检查 diff_sketch
   的文件集合，有交集即串行化。
4. **负优化与 KEEP 并存**：REVERT 轮次照常回滚并记录（回滚记录是瓶颈证据的
   组成部分），不影响 KEEP 轮的仲裁。

### 完成验证

- 每机制 diff_audit.json 存在且 verdict=OK（UNATTRIBUTABLE 轮已处置，未混入对比）
- 每机制 review.md 存在且结论为 PASS / PASS WITH NOTES（FAIL 轮已处置）
- 每机制 comparison.md 存在且三段结论明确
- 精度 fail 或审查 FAIL/UNATTRIBUTABLE 的轮次：性能数字不得进入任何报告结论
- 多 KEEP 时：已按仲裁规则选定新基线并在 REPORT 记录选择依据；其余 KEEP 产物
  已列为下轮候选优先项；并行分发的方案卡 diff_sketch 无同代码区域交集
  （有交集的已串行化执行）

### 约束提醒

- 主 agent 不得合并多机制产物做「组合方案」（用户显式要求除外）
- diff_audit 的 UNATTRIBUTABLE 是机器红线：主 agent 不以「看起来没问题」推翻
  （要推翻只有一条路——文件补进方案卡并说明原子性后重审）
- 停止条件判定在本 Step 完成后统一执行

---

## Step 5：归档汇报

### 调用方式

主 agent 生成 `{output_dir}/REPORT.md`，**六要素缺一不合格**：

1. 环境与源码指纹（SoC/CANN/driver/commit/device/频率）
2. 逐 case 对比表（baseline/optimized/Δ/speedup/精度——全量，禁只展示最快样本）
3. Bound 与指标变化（前后 verdict 关键指标对照）
4. 保留与回滚清单（负优化轮次与原因——回滚记录是瓶颈证据的组成部分）
5. 预估 vs 实测（每机制 speedup_est 对照实测，喂校准回路）
6. 适用边界标注（partial/dry_run/未验证项；soc/模式适用域）

另产出：
- `case_candidate.json`：E 级案例回灌条目（场景指纹/机制/实测/口径）
- `calibration_data.json`：speedup_est vs 实测对照（校准回路输入）
- `report.html`：**可视化报告**（ops-report skill）——批量场景（batch_pipeline
  已产出矩阵目录）时必做；单轮场景可选：

```bash
python3 <ops-verdict>/scripts/render_html.py \
    --matrix-dir {output_dir}/matrix --title "OpsTune 调优报告 — {operator}@{soc}" \
    -o {output_dir}/report.html
```

### 完成验证

REPORT.md 六要素齐全；多轮场景含各轮改进幅度与相对首轮基线的总改进；
批量场景 report.html 存在且与矩阵同数据源（数字不一致 = 渲染 bug，报 issue 不手改）。

---

## 报告格式通用规范

所有阶段产物头部：

```markdown
**状态**: ✅完成 / ❌失败 / ⏭️跳过
**阶段**: 采集与基线 / 分层判定 / 候选方案 / 单轮实施 / 归档汇报
**摘要**: 一句话结论
```

### 逐 case 覆盖格式

```markdown
| Case | Shape | dtype | 瓶颈(rule_id) | 机制 | 基线(us) | 优化后(us) | speedup | 精度 |
|------|-------|-------|--------------|------|---------|-----------|---------|------|
| 1 | ... | fp16 | L1_VEC_BOUND | ub-fusion | 14.1 | 9.2 | 1.53x | PASS |
```

## 故障分流

采集/编译/运行故障 → 读 `workflows/troubleshooting.md`；精度/崩溃/hang → 停止调优，
转精度/崩溃调试域流程（参考 CANN 调试资料），不在本套件范围内处置。
