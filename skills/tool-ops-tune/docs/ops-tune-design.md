# 1. 设计目标与原则

给定一个**可运行且精度通过**的算子实现，在**算子语义不变**的约束下，回答并解决四个问题：

| 问题 | 输入 | 输出 |
|------|------|------|
| **还差多远**（标尺） | 理论上限 / 基线锚点 | 达标判定：值得继续优化吗 |
| **瓶颈是什么**（证据） | profiling 指标 / 隔离实验 | Bound 判定 + 证据链 + 反证 |
| **怎么改**（机制） | 判定结果 + 知识库 | 候选机制（收益预估/风险/回滚条件） |
| **改好了吗**（闭环） | 前后同口径数据 | 对比结论（KEEP/REVERT）+ 经验沉淀 |

## 1.1 三条设计原则

| # | 原则 | 含义 |
|---|------|------|
| P1 | **流程标准化，明确数据契约，各步骤可追溯** | 调优全流程的每个步骤都有明确的输入/输出契约文件（七类 schema，见 §6）——采集落 collection_manifest、判定落 verdict.json、对比落 comparison.md；任何结论都能沿契约链回溯到原始证据（证据数值 + 阈值溯源 + 口径指纹），步骤间不存在口头/隐式传递 |
| P2 | **知识标准化，业界 OKF 格式化，可复用扩展** | 判定规则（YAML 规则包）、机制卡（固定八要素）、案例卡（A-E 证据分级）、算子族（A/B 双型）全部结构化、带路由入口；演进方向为业界 OKF（Open Knowledge Format）标准化，与 CANN-KNOWLEDGE 知识库双向对接——本套件知识可被生态检索复用，外部知识可按同一格式接入 |
| P3 | **确定性下沉、不确定性上浮** | 确定性逻辑（如理论性能标尺、Bound 判定、口径校验、容量计算）沉淀在工具/SKILL 层脚本化承载——可审计、可回归、不随模型波动；不确定性（意图理解、机制选择、争议仲裁）收敛在 Agent/LLM 层处理 |

**派生原则**（由上述三条约束出的工程纪律）：

- 复用不重做：采集底座复用 CANN 官方工具链；事实性知识外部引用不落副本（P2 的引用形态）；
- 同一 SKILL 服务多入口：CLI / IDE / CI / 对话四入口共用同一 SKILL 集与数据契约（P1 的契约复用）；
- Agent 只编排不判定：编排层消费 verdict.json 做调度与门禁，不做 Bound 判定、不写代码（P3 的边界）。

## 1.2 非目标（scope 边界）

- **不做算子开发**：入口前提是「可运行且精度通过」；从零开发走算子开发域工作流。
- **不做精度调试**：精度失败/崩溃/挂起 → 停止调优，转调试域（出口承接）。
- **不重做采集器**：msprof / npu-compute / cannsim 全部复用，本套件只做模式路由与契约落盘。

---

# 2. 总体架构：四层

| 层 | 职责 | 关键原则 |
|----|------|---------|
| L4 用户场景层 | 单算子 / 融合算子（FA·MC²）/ 批量回归 / 调优问答，四入口 | 四入口共用同一 SKILL 集与数据契约（P1） |
| L3 Agent 编排层 | OpsTuningAgent 调优主循环 + perf-implementer / perf-reviewer | 只编排不判定（P3） |
| L2 SKILL 能力层 | ops-collect / ops-verdict（★核心）/ ops-mechanism / ops-family / ops-report | 确定性沉淀、可独立使用；判定只出自脚本（D1） |
| L1 工具层 | msprof / npu-compute / cannsim / npu-smi / CANNBench | 复用不重做（派生原则） |

**数据流主线**（每个节点都是有 schema 的契约文件，见 §6）：

```
工程 + cases
    ▼ ops-collect            collection_manifest.json（采集块状态机真源）
    ▼ ops-verdict verdict.py verdict.json（判定书：findings + 反证 + 机制路由）
    ▼ snapshot.py            baseline/result.json（口径指纹）
    ▼ ops-mechanism/family   candidates.json（≤3 张候选方案卡）
    ▼ perf-implementer       round-N/result.json（单机制优化副本）
    ▼ delta_compare.py       comparison.md（KEEP/REVERT/复测三段结论）
    ▼ ops-report             REPORT.md + report.html + E 级案例回灌条目
```

---

# 3. 核心设计决策（D1-D10）

## D1 判定进脚本、解释进 LLM（架构第一原则）

**问题**：Bound 判定靠 LLM 阅读文档执行——判定质量随模型/版本波动，阈值变更不可回归，无证据宣称无法根除。

**决策**：判定 = `ops-verdict` 规则引擎（声明式 YAML 规则 + 脚本求值）。LLM 只做三件事：
①编排（调度、门禁、汇报）；②解释（把 finding 的 evidence/counter_evidence 翻译给人听）；
③机制选择（在 ≤3 张候选方案卡中决策）。

**边界**：LLM 不得输出规则库之外的判定；规则未覆盖的形态只能作为假设并标注缺失指标
（INSUFFICIENT_DATA 语义），不得写成结论。

## D2 规则-指标绑定契约（判定质量的机制保障）

**问题**：「指标没采却报出瓶颈」是判定误报的主要来源；纪律性约束无法机械化执行。

**决策**：每条规则声明两级指标绑定——`metrics_required` 缺失 → 规则 **SKIPPED**
（不触发、不误报、留补采提示）；`metrics_optional` 缺失 → 降级评估（反证标 unevaluated）。
把「无证据不判定」从纪律变成**机制**。

## D3 分层瀑布 + 路由副作用（瓶颈污染的架构解）

**问题**：MC² 场景 MTE2 ratio 被通信等待污染，单核访存优化做了白做。

**决策**：判定按 **L3 卡间 → L2 核间 → L1 单核 → L0 指令** 依序执行，允许路由副作用：

```
L3_MTE2_POLLUTED 命中（ratio_full−ratio_pure > 10pp 或 pure/full < 80%）
    ├─ 禁用 L1_MTE2_BOUND（跨规则包传播）
    └─ 改道机制路由 → mc2.md 通信掩盖策略
```

**配套语义——条件三值逻辑**：条件求值 ∈ {True, False, **None**}；None = 证据不足 →
INSUFFICIENT_DATA，**绝不因数据缺失而判「无 bound」**（None 不冒充 False）。

## D4 口径指纹互锁（同口径铁律的数据结构化）

**决策**：`snapshot.py` 每次测量落口径指纹（HARD_KEYS 12 字段 + MECHANISM_KEYS 2 字段 +
精度指纹 + status 三态）；`delta_compare.py` 三道硬校验（任一失败 exit 2 拒绝生成对比）：
①口径硬校验（HARD_KEYS 不一致 → 拒绝比较）；②精度门禁（任一侧 precision ≠ pass → 拒绝）；
③噪声带三段结论（speedup > 1.05 KEEP / < 0.95 REVERT / 带内 → repeat 翻倍复测不宣称）。

**语义细节**：`tiling_key` / `block_dim` 是分核/tiling 类优化**本身就会改变**的变量，从
「禁止比较」降级为「WARN 写入报告」——对优化本质的正确建模。

## D5 调优主循环 + 单变量铁律

三条铁律全程生效：
1. **精度三道门禁**：进入（意图与边界步骤全集跑通）/ 每轮（实施后先精度后性能）/
   结论（delta_compare 脚本化）；
2. **单变量归因 + 负优化四机制**：可预防（候选须有当前算子 profiling 证据）/ 可检测
   （同口径 + 噪声带）/ 可回滚（diff + 备份 + 回滚后复跑验证）/ 可交代（回滚记录进报告
   ——负优化记录是瓶颈证据的组成部分）；
3. **停止条件不硬编码轮数**：达标 / 候选尽 / 连续两轮无稳定收益 / 接近上限（NEAR_LIMIT）。

## D6 知识按消费时机分层（渐进式披露）

```
采集前   tool-routing.md（工具选型决策树）
判定后   机制卡（findings[].mechanism_refs 精确路由）——判定之前禁止加载机制/案例
实施时   族 guide + 模板链路（模板拷贝纪律 T1-T5）
归档时   案例卡 schema + 证据分级 + 降级策略
```

配套纪律：SKILL.md 只做路由与触发；每文档头部声明适用边界（soc/模式/形态）；
「暂未收录」显式返回并计数（缺口感知回路输入）。

## D7 职责隔离的编排

| 隔离 | 设计 |
|------|------|
| 分析者不判定 | 判定出自 verdict.py，Agent 只消费 |
| 实施者单机制 | perf-implementer 一次领一张方案卡（可归因） |
| 主 agent 垄断统一采集与 delta_compare | 各机制若自采则口径漂移 |
| diff 范围机器校验 | diff_audit.py 比对实际改动 ⊆ 方案卡声明，超范围 UNATTRIBUTABLE 禁进 KEEP 仲裁 |
| 实施独立审查 | perf-reviewer 独立重编 + 单变量符合性 + 模板保真，结论四档 |

## D8 执行式评测（判定质量的回归保障）

判定进脚本后评测跟着进脚本：`evals/run_evals.py` 用 fixture（已知答案的 CSV/标量输入，
源自真机调优案例）断言 verdict 输出，覆盖正例/反例/边界/绑定契约/路由副作用
（含 R 值临界值 0.90/1.10 双侧守护）。退出码 1 = 阻断合入（CI 语义）。

## D9 外部知识动态引用（应对芯片/API 演进）

知识按「是否随芯片/API 演进」分两类：事实性知识（芯片参数/API 签名/官方示例/理论标尺）
引用 CANN 生态真源动态更新，本套件只存引用清单 + 版本锁定；经验性知识（指标→瓶颈→机制
因果）不随 API 演进失效，自建沉淀。芯片换代成本被外部真源吸收，演进面收敛在适配层
（列名兼容/soc 映射）。

## D10 场景路由机制（五级路由瀑布）

| 级 | 路由轴 | 决策载体 | 错路由的后果 |
|----|--------|---------|-------------|
| R1 | 调优场景 | AGENTS.md 场景表 | MC² 走单算子流程 → 污染未检 → 单核误判 |
| R2 | 采集模式 | tool-routing 决策树 | fork 程序走 msprof op → 数据不可靠 |
| R3 | 判定层次 | 规则包 YAML（layer/priority/soc） | 层序颠倒 → 路由副作用失效 |
| R4 | 优化机制 | mechanism_refs | 证据先行被破坏 |
| R5 | 算子族 | 族分类表 + 模板链路 | broadcast 三实现选错直接慢/nan |

每级路由的决策依据都是结构化产物，LLM 只查表执行不发明路由——**路由进表格，不进 LLM 记忆**。

---

# 4. 调优主循环与各阶段产物

主循环五步：**采集 → 问题判定 → 机制路由候选方案 → 单变量执行优化 → 对比确认**
（外层为意图边界与归档回灌两个编排步骤；循环体内每轮单变量迭代后回到对比确认，
连续两轮无稳定收益或达标即收敛）。

```
（前置：意图与边界——编排步骤，非循环体）
    ├─ 精度门禁预检：工程正确性全集跑通？失败 → 拒绝，转调试域
    ├─ 场景分类：单算子 / 融合算子（FA·MC²）/ 批量回归 / 问答
    └─ 产物：scope.json
┌───────────────────────── 调优主循环 ─────────────────────────┐
│ Step 1 采集（ops-collect）                                      │
│    ├─ 环境指纹 + 工具路由（MC² → task-based 强制）              │
│    ├─ 不可变基线 snapshot（HARD_KEYS 12 字段，首轮建立）         │
│    └─ 产物：collection_manifest.json + result.json             │
│ Step 2 问题判定（ops-verdict）                                  │
│    ├─ 标准 7 组指标 或 MC² 隔离测试（isolation_test → R 值）    │
│    ├─ verdict.py：L3→L2→L1→L0 瀑布；INSUFFICIENT_DATA → 补采    │
│    └─ 产物：verdict.json（findings + 反证 + 置信度 + 机制路由）  │
│ Step 3 机制路由候选方案（ops-mechanism + ops-family）           │
│    ├─ 按 mechanism_refs 加载机制卡（渐进式披露）+ 同型案例 ≤3    │
│    └─ 产物：candidates.json（≤3 张方案卡）                      │
│ Step 4 单变量执行优化（perf-implementer + 机器闸门）            │
│    ├─ 每方案卡一个 implementer（复制目录→模板拷贝→编译→全量精度）│
│    ├─ diff_audit.py 范围校验 + perf-reviewer 独立审查           │
│    └─ 产物：round-N/{优化副本, diff_audit.json, review.md}      │
│ Step 5 对比确认（主 agent 垄断统一采集 + delta_compare）        │
│    ├─ 同口径采集（与基线同一 timing/warmup/repeat/device）       │
│    ├─ 三段结论：KEEP（保留为新基线）/ REVERT（回滚复跑）/        │
│    │            噪声带内（repeat 翻倍复测，不宣称收益）           │
│    └─ 产物：round-N/{result.json, comparison.md}                │
│    未收敛 → 回 Step 1（以 KEEP 轮为新基线，重新采集与判定）       │
└───────────────────────────────────────────────────────────────┘
（收尾：归档回灌——编排步骤，非循环体）
    ├─ REPORT.md 六要素 + report.html
    └─ 产物：REPORT.md + case_candidate.json（E 级回灌）+ calibration_data.json
```

**停止条件**（不硬编码轮数）：达标 / 候选尽 / 连续两轮无稳定收益 / 接近上限（NEAR_LIMIT）。

## 4.1 各阶段产物契约总表

| 步骤 | 产物 | schema | 生产者 → 消费者 |
|------|------|--------|----------------|
| 前置 | scope.json | opstune.scope/1.0 | 主 agent → 全流程（口径互锁上游真源） |
| 采集 | collection_manifest.json | opstune.collection/1.0 | collect → verdict/报告（采集块状态机） |
| 采集 | baseline/result.json | opstune.snapshot/1.0 | snapshot → delta_compare（不可变基线） |
| 问题判定 | verdict.json | opstune.verdict/1.1 | verdict → Agent/报告/CI（判定书） |
| 问题判定 | iso_result.json | opstune.isolation/1.0 | isolation_test → verdict --scalar（MC²） |
| 机制路由 | candidates.json | —（方案卡字段集） | mechanism → implementer（≤3 张） |
| 对比确认 | round-N/result.json | opstune.snapshot/1.0 | snapshot → delta_compare |
| 对比确认 | comparison.md | — | delta_compare → REPORT（三段结论） |
| 执行优化 | diff_audit.json | opstune.diff-audit/1.0 | diff_audit → reviewer（范围机器判定） |
| 收尾 | REPORT.md / report.html | —（六要素） | 主 agent → 用户 |
| 收尾 | case_candidate.json | —（E 级字段集） | REPORT → cases/（回灌候选） |
| 收尾 | calibration_data.json | — | REPORT → calibrate.py（预估-实测校准） |

**单一数据源原则**：REPORT（人读）、Agent 概览（对话）、CI 检查（JSON 断言）从同一批
契约文件渲染，不存在第二套数字。

---

# 5. 判定引擎详细设计

## 5.1 规则包结构（opstune.rules/1.0）

```yaml
schema: opstune.rules/1.0
layer: L1                     # L3 卡间 / L2 核间 / L1 单核 / L0 指令
soc: [a2, a3, a5]             # 芯片域硬过滤
metrics:                      # 指标词汇表（绑定契约的解析层）
  mte2_ratio:
    source: pipe              # pipe（CSV 逐行聚合）/ scalar / derived
    columns: [aic_mte2_ratio, aiv_mte2_ratio, ai_mte2_ratio, ...]  # 三代列名兼容
    normalize: ratio_to_pct   # 0~1.5 视为比例 ×100
rules:
  - id: L1_MTE2_BOUND                # 全局唯一（跨包路由引用）
    priority: 10                     # 升序求值
    threshold_source: "[msprof-doc] >80% 直接判 / 最大且 >70%"   # 阈值必溯源
    metrics_required: [mte2_ratio]           # 缺失 → SKIPPED（绑定契约）
    metrics_optional: [mte2_active_bw_pct_of_peak]  # 缺失 → 降级
    condition:                       # any/all 递归三值逻辑
      any:
        - {metric: mte2_ratio, agg: mean, op: gt, value: 80}
        - all:
            - {metric: mte2_ratio, agg: mean, op: gt, value: 70}
            - {metric: mte2_ratio, agg: rank, op: eq, value: 1}   # 主导 pipe
    severity: {metric: mte2_ratio, bands: [{min: 85, level: critical}, {min: 70, level: moderate}]}
    counter_evidence:                # 反证条目（BOUND 类必带，防误判第二道闸）
      - id: true-bandwidth-saturation
        condition: {metric: mte2_active_bw_pct_of_peak, op: gt, value: 80}
        note: 真带宽饱和：优化在数据量/复用，不在搬运组织
    confidence: high
    speedup_range: [1.2, 2.0]        # 预估收益（校准闭环回写）
    mechanism_refs: [ops-mechanism:references/optimize/data-copy.md]
    routing: {on_hit: [disable_rule, L1_MTE2_BOUND]}   # 路由副作用（跨包传播）
```

## 5.2 求值语义

| 环节 | 语义 |
|------|------|
| pipe 指标 | 逐行取候选列（按序首个可解析）→ aic_/aiv_ 双列最大值 → 跨行 mean/max 双统计；**判定用 mean，max 仅识别长尾** |
| 主导 pipe | `agg: rank, op: eq, value: 1` |
| 串行指纹 | `__pipe_sum_mean ∈ [85,115]`（多 pipe 互补求和≈100% → 单缓冲串行） |
| 逐核长尾 | max > mean × 1.25 |
| R 值 | R = T_compute/T_comm 五档（[0.9,1.1] 平衡 / >2 强计算 / <0.5 强通信…） |
| MTE2 污染 | full−pure > 10pp 或 pure/full < 80% → 禁用单核 MTE2 判定 |

## 5.3 规则状态机与汇总三态

```
REQUIRED 缺失 → SKIPPED ｜ 条件 None → INSUFFICIENT_DATA ｜ False → NOT_HIT ｜ True → HIT
汇总：NEEDS_OPTIMIZATION（有 HIT）/ NEAR_LIMIT（评估过无 HIT）/ INSUFFICIENT_DATA（禁止输出结论）
```

## 5.4 首期规则清单（24 条）

| 层 | 规则 |
|----|------|
| L1 单核（8） | MTE2/CUBE/VEC/MTE3/FIXP/SCALAR BOUND、LATENCY_SYNC_SUSPECT、SERIAL_NO_OVERLAP |
| L2 核间（6） | CORE_TAIL、IMBALANCE_MILD/SEVERE、UNDERUTILIZATION、HEAD_OVERHEAD、HOST_DISPATCH_BLOCK |
| L3 卡间（7） | MC2_BALANCED / COMPUTE_BOUND / STRONG_COMPUTE / COMM_BOUND / STRONG_COMM、MTE2_POLLUTED（路由）、RANK_IMBALANCE |
| L0 指令（3） | ICACHE_MISS、L2_LOW_HIT、STALL_DOMINANT |

扩展方向：FA 混合绑核档、SIMT/VF 档、通信带宽档；远期父子 DAG 权重传播。

---

# 6. 数据契约体系

七类契约构成全部交付物的数据底座（字段与最小示例见各子域 references/ 下的
io-contracts / rule-schema / scope-schema）：

| 契约 | schema | 关键字段 |
|------|--------|---------|
| 采集清单 | opstune.collection/1.0 | blocks[]{name,status,path}；已有 manifest 禁重复采集；禁止伪 Green |
| 判定书 | opstune.verdict/1.1 | findings[]{rule_id,severity,confidence,evidence,counter_evidence,speedup_range,mechanism_refs} |
| 结果指纹 | opstune.snapshot/1.0 | HARD 12 口径字段 + MECHANISM 2 字段 + precision + status 三态 |
| 隔离测试 | opstune.isolation/1.0 | t_comm_us/t_compute_us/r_value/bound_hint（跨 rank max 口径） |
| 方案卡 | candidates.json | mechanism/target_rule/evidence/speedup_est/semantic_risk/rollback_if/template |
| 实施审查 | opstune.diff-audit/1.0 | changed_files vs 声明范围；UNATTRIBUTABLE 退出码 2 |
| 案例回灌 | case_candidate.json | 场景指纹 + 机制 + 实测 + 口径（E 级） |

**契约演进规则**：新增字段必须可选（向后兼容）；删除/改义字段升 minor 版本；
消费方同步更新并过 evals。

---

# 7. 知识工程体系

## 7.1 五层知识（K1-K5）

| 层 | 内容 | 形态 | 扩展方式 |
|----|------|------|---------|
| K1 判定规则 | 触发条件+反证+置信度+收益预估 | YAML 规则包（脚本求值） | 新规则 = YAML + fixture + evals 断言，全绿合入 |
| K2 优化机制 | 机制卡八要素（原理/前提/收益/架构限制/精度风险/验证命令/回滚条件/模板资产） | 机制卡 + rule_id 索引 | 八要素写全 + 挂 mechanism_refs |
| K3 算子族与模板 | 族范式 + 选型决策树 + 可拷贝代码 | A/B 双型文档 + 模板货架 | 建族目录 + 模板链路；追上游重拷不分叉 |
| K4 案例证据 | 场景指纹 + 实测 + 证据分级 A-E | 案例卡（反例同等收录） | 实测回流（E）+ 官方 PR（A/B）入库评审 |
| K5 外部知识引用 | 芯片参数 / API 文档 / 理论标尺 | 引用清单 + 版本锁定，不落副本 | 引用 bump + CI 可达性校验 |

## 7.2 知识纪律与自演进

- **阈值纪律**：每条阈值带 threshold_source（msprof-doc / tuning-practice / mc2-practice /
  首期标定 🟡）；变更流程 = 改 YAML → evals 全绿 → 来源标注 → 人审合入；无来源降级
  watch 档，缺反证的 BOUND 类不允许合入；
- **自演进四回路**：案例回流 / 校准闭环（speedup_est vs 实测回写）/ 阈值进化（命中率
  统计 → 人审 + 回归全绿才生效）/ 缺口感知（「暂未收录」计数 → 排产信号）；
- **防污染四纪律**：进化只产生建议不直接生效 ｜ 证据分级不可逆（E 级不覆盖 A 级）｜
  数据脱敏 ｜ 基线库版本化。

---

# 8. 关键场景端到端走查

## 8.1 单算子（Vector 类，如 GELU）

```
前置: 正确性全集 pass → scope.json
Step 1 采集: collect_board --mode standard（7 组指标）→ manifest + baseline snapshot
Step 2 问题判定: verdict.py（L2+L1）→ L1_VEC_BOUND 命中 + cast-heavy 反证
Step 3 机制路由: mechanism_refs → api-usage.md（减 Cast）→ 2 张方案卡（模板✅/⚠️）
Step 4/5 执行+对比: implementer 实施 → diff_audit OK → reviewer PASS → 统一采集
         → delta_compare：14.1→9.2us speedup 1.53x KEEP
收尾: REPORT 六要素 + E 级案例候选 + 校准数据
```

## 8.2 融合算子（MC²，如 alltoall_matmul）

```
前置: 识别 fork 多 rank + CrossCoreFlag → 场景=MC²；rankNum≤空闲卡数校验
Step 1 采集: collect_board --mode mc2（task-based 强制）
Step 2 问题判定: ① isolation_test（源码副本注释编排）→ T_comm=420us T_compute=880us R=2.10
         ② 全瀑布 verdict：L3_MC2_STRONG_COMPUTE + L3_MTE2_POLLUTED 命中
            → L1_MTE2_BOUND 自动禁用（路由副作用的价值时刻）+ L2_IMBALANCE_SEVERE
Step 3 机制路由: mc2.md（Local Matmul 首选）+ mc2-balancing.md + mc2_search.py 输出
Step 4/5 执行+对比: 配平候选逐个实测（跨 rank max 口径）→ 择优 KEEP
收尾: REPORT + E 级案例（MC² 场景指纹 → cases/mc2/）
```

## 8.3 调优问答（已有产物）

仅「问题判定」步骤离线执行 + 报告解读：manifest 存在只解析不重采；禁止无证据输出 Top5；
数据不足按真实数量输出，不补齐不造数。

---

# 9. 扩展指南

## 9.1 新增一条判定规则（最常见扩展）

```
1. rules/<layer>-*.yaml 追加规则（threshold_source 必填；BOUND 类必带反证）
2. fixtures/ 造已知答案输入；evals/run_evals.py 加断言（正例 + ≥1 反例/边界）
3. python3 evals/run_evals.py 全绿
4. 若引入新机制方向 → ops-mechanism 补机制卡并挂 mechanism_refs
```

## 9.2 新增一个算子族 / 一个 DSL / 接入编排平台

- **新族**：`ops-family/references/<family>/` → 判 A/B 型 → guide + 模板链路 → 族表更新；
- **新 DSL**（CATLASS/Triton/TileLang 等）：判定引擎零改动（规则层 DSL 无关）；
  ops-mechanism 跨 DSL 原语对照表路由；ops-family 增 DSL 分册；
- **编排平台**：agent/ 即插件形态（AGENTS.md primary + agents/ + workflows/）；
  SKILL 可独立使用（CLI/CI）是兜底形态。

## 9.3 Roadmap

| # | 方向 |
|---|------|
| 1 | 性能报告解读 SKILL（AscendC Toolkit 插件：性能问题 + 证据 + 修复建议，引用本套件判定引擎与机制卡） |
| 2 | 补充最佳实践案例（A/B 级官方 PR + E 级实测回流） |
| 3 | 优化 HTML 报告（交互式钻取 / Bound 演化时间线 / 多轮对比视图） |
| 4 | ~~采集工具切换 DB 格式数据~~ ✅ 已交付（npu-compute 产物 sqlite 化：采集编排 --mode npu-compute / facts_bridge DB 通道 / verdict --db / csv_to_db 升级器 / db_bench 等价性门禁；PcSampling → top_stall_pct 打通 L0_STALL_DOMINANT） |
| 5 | 知识标准化 OKF 格式对接 CANN-KNOWLEDGE（知识资产可被生态检索复用） |
| 6 | AGENT/SKILL 增加 trace 能力（判定与调优决策链全程留痕可视化） |
| 7 | 新芯片 / API 适配（soc-spec / 规则包 soc 域 / 列名兼容层随芯片演进） |

---

*OpsTune 算子调优 AGENT 架构设计文档 · V2.0 ｜ 判定进脚本 · 解释进 LLM · 经验可回流*
