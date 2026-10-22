# 算子调优 AGENT 架构设计文档（OpsTune）

> **文档定位**：OpsTune 算子调优 Agent 的完整架构设计——本套件开发的
> 单一权威设计依据。
>
> 版本：V1.2 ｜ 日期：2026-10-19 ｜ 状态：M1 骨架已交付（判定引擎可运行、evals 全绿）
> V1.2 变更：开源版适配——目录结构按 skills/tool-ops-tune 套件布局更新（§12.2），去除内部排期与人力信息
> V1.1 增补：D9 外部知识动态引用 ｜ D10 五级路由 ｜ K5 引用层 ｜ 演进路线
>
> 配套文档：[算子调优AGENT规划文档.md](算子调优AGENT规划文档.md)（场景与价值）｜ 《算子调优AGENT用例构建.md》（评测体系，随测试框架 PR 交付后链接恢复）
>
> **一句话**：以「标尺—证据—机制—闭环」为方法论，把判定下沉为脚本化规则引擎
>（规则-指标绑定契约），把编排上收为只调度不判定的 Agent，把知识按消费时机分层
>（渐进式披露），把评测做成执行式回归——**判定进脚本、解释进 LLM、经验可回流**。

---

## 目录

- [1. 背景与问题定义](#1-背景与问题定义)
- [2. 设计目标与原则](#2-设计目标与原则)
- [3. 友商对标：Nsight Compute Guided Analysis](#3-友商对标nsight-compute-guided-analysis)
- [4. 总体架构：四层](#4-总体架构四层)
- [5. 核心设计决策（D1-D10）](#5-核心设计决策d1-d10)
- [6. Agent 层设计](#6-agent-层设计)
- [7. 判定引擎详细设计](#7-判定引擎详细设计)
- [8. SKILL 层设计](#8-skill-层设计)
- [9. 数据契约体系](#9-数据契约体系)
- [10. 知识工程体系](#10-知识工程体系)
- [11. 关键场景端到端走查](#11-关键场景端到端走查)
- [12. 目录结构与能力构成](#12-目录结构与能力构成)
- [13. 扩展指南](#13-扩展指南)
- [14. 演进路线摘要](#14-演进路线摘要)
- [15. 风险与对策](#15-风险与对策)
- [16. 附录](#16-附录)

---

# 1. 背景与问题定义

## 1.1 算子调优任务的本质

给定一个**可运行且精度通过**的算子实现，在**算子语义不变**的约束下，回答并解决四个问题：

| 问题 | 输入 | 输出 |
|------|------|------|
| **还差多远**（标尺） | 理论上限 / 基线锚点 | 达标判定：值得继续优化吗 |
| **瓶颈是什么**（证据） | profiling 指标 / 流水时序 / 隔离实验 | Bound 判定 + 证据链 + 反证 |
| **怎么改**（机制） | 判定结果 + 知识库 | 候选机制（收益预估/风险/回滚条件） |
| **改好了吗**（闭环） | 前后同口径数据 | 对比结论（KEEP/REVERT）+ 经验沉淀 |

本 Agent 的全部设计都围绕这四个问题的**质量、可复现、可规模化**展开。

## 1.2 七类现状痛点

| # | 痛点 | 现状根因 | 代价 |
|---|------|---------|------|
| 1 | **专家依赖**：MTE2 Bound / VF 诊断需资深专家约 1 人月/次 | 指标是原始 PMU 数值，无自动瓶颈判定规则引擎；过程不可见不可复现 | 调优吞吐受人头限制，经验难传承 |
| 2 | **无理论标尺**：不知道离硬件极限还差多少 | Roofline 存在但非默认不贯穿；无常驻「达上限百分比」读数 | 调优方向模糊，值不值得做无判据 |
| 3 | **无基线记忆**：迭代前后对比靠人工记录 | 输出是一次性 CSV，无版本化基线、无跨 run diff | 回归问题到集成阶段才暴露 |
| 4 | **数据链路割裂**：时间线/指标/源码各自独立产物 | 关联键只在解析层，未贯通到体验层 | 定位问题靠人工跨文件比对 |
| 5 | **采集门槛高**：多卡/融合算子采集依赖专家经验 | 能力存在但暴露为多步手工命令行；MC² 必须 task-based 这类路由规则散在文档 | 关键指标「能采但采不到」或「采错」 |
| 6 | **融合算子近乎盲区**：FA / MC² 无体系化诊断路径 | 知识库按单范式算子组织；跨核/跨卡瓶颈相互污染 | 只能凭通用原理推导，细节需自行证明 |
| 7 | **调优链路手工编排**：一次调优要人工串 6+ 个 SKILL | SKILL 是散装原子能力，无统一编排契约；评测全 text 模式 | 流程不可重复，质量靠运气 |

## 1.3 融合算子三大深层难点（架构硬约束）

融合算子是难中之难：**FA 混合绑核**（1 AIC Cube + 2 AIV Vector，CrossCore mode2 跨核同步，跨 j 迭代 GM buffer ping-pong）与 **MC² 通算融合**（AIC matmul + AIV UDMA/shmem 跨卡通信，多 rank 木桶效应）。三大深层难点直接决定本架构的取向：

**① 瓶颈污染**

多执行单元（AIC/AIV/MTE/通信核）混布，单一 pipe 占用率无法直接归因：

- MC² 场景：`CrossCoreWaitFlag` 挂在 AIC 的 MTE2 流水线等待 AIV 通信 → `aic_mte2_ratio` 含通信等待空转，单核视角判 MTE2 Bound 是误判；
- FA 场景：AIV 的 MTE2 等待可能来自 AIC 未就绪（CrossCore 依赖）而非带宽不足。

> **架构硬要求**：判定引擎必须**分层 + 带前置检查**，不是单层阈值表。上层结论（污染判定）必须能约束下层（禁用单核访存判定）。

**② 跨层耦合**

卡间切分 ↔ 核间切分 ↔ 单核流水三层联动：改一个 tiling 参数同时影响通信粒度、核利用率、L1/L0 容量。单变量迭代仍然成立，但归因必须有层次。

> **架构硬要求**：判定流程必须**分层瀑布**（L3 卡间 → L2 核间 → L1 单核 → L0 指令），上层结论作下层输入。

**③ 无统一标尺**

融合算子理论上限是**多路径的**（通信/计算/访存各一条 roof），跨 rank 木桶效应让「单核达标」≠「算子达标」：

- MC² 整体性能 = 最慢 rank（各 rank 稳态均值的 **max**，非平均）；
- 掩盖率 =（融合前耗时 − 融合后耗时）/ min(计算耗时， 通信耗时)，上限 1.0。

> **架构硬要求**：标尺升级为多维 roof + 跨 rank max 口径；R = T_compute/T_comm 实验测定（实测跨 0.3~9.8，30 倍区间——**每 case 必须实测，禁止沿用**）。

## 1.4 业界现状盘点结论（问题空间的证据）

对业界算子调优工具链（SKILL 化调优知识与 Agent 编排两类形态）的调研给出六个判断：

| # | 判断 | 现状证据 | 对本架构的含义 |
|---|------|---------|---------------|
| 1 | **知识深度与流程广度长期分裂**：知识库深的缺流程纪律（MC² 方法论、算子族模板、插件编排），流程全的缺知识深度（多 DSL 覆盖、案例证据分级、闭环纪律、计时口径） | 两类资产几乎不重叠 | 本架构知识层与流程层一体化设计，不二选一 |
| 2 | **判定普遍未脚本化成体系** | 阈值散在文档靠 LLM 阅读执行；个别初筛脚本仅覆盖少数 Bound 类型，无 DAG/置信度/融合档/芯片域参数化 | **判定引擎是第一优先级增量** |
| 3 | **评测普遍全 text**：判定质量零回归保障 | 调优类评测以文本问答为主，无「给定指标数据断言判定输出」的执行式回归 | 判定进脚本后，评测必须跟着进脚本（执行式） |
| 4 | **编排职责隔离方向正确但不完整** | 「分析者不改码 / 实施者单方案 / 主 agent 垄断统一采集」已在实践验证；但普遍仅覆盖单一 DSL | 编排层继承职责隔离设计，SKILL 化使其不绑单 DSL |
| 5 | **知识无版本契约、经验不回流** | 阈值/收益散在文档无 schema 无芯片域标注；案例库依赖人工编写入库 | 指纹契约 + 阈值溯源 + 回流条目是标配 |
| 6 | **融合算子是知识最强与最弱的分水岭** | MC² 方法论（隔离测试/R 值五档/配平/膨胀/抖动）长期停留在文档形态零自动化；FA 类混合绑核普遍列为知识缺口 | MC² 方法论脚本化（隔离测试编排）；FA 走降级策略 + 案例沉淀 |

业界共性短板：判定引擎缺位、执行式评测缺位、知识无契约、经验不回流、工具链之间缝隙（引用他方 agent 名但无人履约）。**本架构的四个主攻方向正是这四条共性短板。**

---

# 2. 设计目标与原则

## 2.1 方法论：标尺—证据—机制—闭环 四位一体

```
┌─────────────────────────────────────────────────────────────────────┐
│ 标尺（还差多远）   理论上限 t_hw/多维 roofline + 基线锚点 + 跨 rank max  │
│      ▲ 决定投入方向                                                  │
│ 证据（瓶颈是什么） 八类数据集 + 分层判定（含反证与置信度）                │
│      ▲ 决定机制选择                                                  │
│ 机制（怎么改）     机制卡八要素（收益预估/风险/回滚条件）+ 模板货架       │
│      ▲ 单变量实施                                                    │
│ 闭环（改好没）     同口径对比 + 噪声带三段结论 + 精度门禁 + 经验回灌      │
└─────────────────────────────────────────────────────────────────────┘
```

一句话方法论：**算子调优 = 在「算子语义不变」约束下，以标尺—证据—机制—闭环四位一体驱动的分层归因与单变量迭代。**

## 2.2 五条架构原则

| # | 原则 | 含义 | 反例（现状） |
|---|------|------|-------------|
| P1 | **判定进脚本、解释进 LLM** | 判定、阈值、口径校验全部由脚本承载（可审计、可回归、可版本化）；LLM 只做编排、解释与机制选择 | Bound 判定靠 LLM 读 md 文档执行，质量随模型波动 |
| P2 | **确定性下沉、不确定性上浮** | 确定性逻辑沉淀在 L2 SKILL 层（规则/脚本/契约）；不确定性收敛在 L3 Agent 层（意图理解/争议仲裁） | 判定这种确定性工作散落在 LLM 执行路径上 |
| P3 | **复用不重做** | 采集底座复用 CANN 官方（msprof/msopprof/cannsim）；评测复用 CANNBench；**事实性知识（芯片参数/API/示例/标尺）外部引用不落副本（D9）**，判定与机制知识自建并版本化沉淀在本仓 | 重造采集器；维护随芯片/API 演进腐烂的知识副本 |
| P4 | **同一 SKILL 服务多入口** | CLI / IDE / CI / 对话四入口共用同一 SKILL 集与数据契约 | 每个入口一套逻辑 |
| P5 | **Agent 只编排不判定** | 编排层消费 verdict.json 做调度与门禁，不做 Bound 判定、不写代码 | 主 agent 凭经验给优化建议 |

## 2.3 非目标（scope 边界）

- **不做算子开发**：从零开发/迁移走算子开发域工作流（如 CANN 开发流程）；本 Agent 入口前提是「可运行且精度通过」。
- **不做精度调试**：精度失败/崩溃/挂起 → 停止调优，转精度/崩溃调试域（出口承接，不是不管）。
- **不重做采集器**：msprof / msprof op / msopprof / cannsim / torch_npu.profiler 全部复用，本 Agent 只做**模式路由与契约落盘**。
- **不卷指标数量**：对标 NCU 7K+ 指标的差距在「组织与契约」层不在采集能力层；补契约与最小充分集，不补数量。

---

# 3. 友商对标：Nsight Compute Guided Analysis

## 3.1 六大资产与本架构的映射

| NCU 资产 | 做法 | 本架构落点 |
|----------|------|-----------|
| SOL（Speed of Light） | Compute/Memory 相对理论峰值百分比常驻 | 2026 出口：CANNBench t_hw 标尺接入 Phase 1「还差多远」读数 |
| **Guided Analysis 规则引擎**（24 条 Python 规则，2026.2.1.5 实测） | 规则自动应用输出 Recommendations | **ops-verdict 判定引擎**（本架构核心增量） |
| Est. Speedup 排序 | 每条建议带预估加速比，按此排序 | 规则字段 `speedup_range` + 候选方案卡排序 |
| Baseline / Source Comparison | 任意结果设基线，报告显示 diff | snapshot + delta_compare（口径指纹互锁） |
| Source-Level Profiling | SASS↔源码逐行指标、stall 采样 | L0 层规则（PcSampling，框架期扩展） |
| Memory Chart / Occupancy Calc | 内存单元图 + occupancy 计算器 | —（不做，昇腾侧按指标表承载） |

## 3.2 规则引擎的四项机制（直接对标的部分）

| NCU 机制 | 本架构实现 |
|----------|-----------|
| **规则-Section 绑定契约**（规则声明绑定 section，未采集则规则不出现） | `metrics_required` 缺失 → SKIPPED；机制上杜绝「指标没采却报规则」 |
| **指标 REQUIRED/OPTIONAL 分级**（同一规则跨架构复用） | 同名两级：REQUIRED 缺失跳过 / OPTIONAL 缺失降级评估（反证标 unevaluated） |
| **父子规则 DAG + 权重传播**（子规则 LOCAL 加速比在父 bound 时提升为 GLOBAL） | `parent` 字段已预留（M1 平铺 + priority；权重传播自进化期启用） |
| **规则结果持久化 + Python 读回**（rule_results_as_dicts()） | verdict.json 即「报告内持久化」：Agent 概览 / HTML / JSON / CI 从同一数据源渲染 |

## 3.3 对标核心判断

NCU 的 Guided Analysis **不是 AI**——它是数据驱动的确定性规则引擎，Copilot 只做解释。「判定进脚本、解释进 LLM」与 NCU 完全同构，这是可对标可复制的部分；而 **MC² 通算流水图是 NCU 没有的等价物**（NV 靠 Nsight Systems 补位），应放大而非补齐 per-link 指标。

---

# 4. 总体架构：四层

## 4.1 四层架构图

![OpsTune 四层架构](figures/opstune-architecture.svg)

> 图件版本：DWG-OPS-001 · REV 1.0（2026-10）。高清图件：[SVG](figures/opstune-architecture.svg) ｜ [PNG](figures/opstune-architecture.png)

图中除 L4-L1 四层外，还绘出两条贯穿主线：**数据契约流**（collection_manifest → verdict.json → candidates.json → result.json → comparison.md → REPORT.md，见 §4.2 / §9）与 **L3 五阶段主循环**（P0-P5，含精度门禁①②与 REVERT 回滚环，见 §6.2）。四层职责一览：

| 层 | 职责 | 关键原则 |
|----|------|---------|
| L4 用户场景层 | 单算子 / 融合算子（FA·MC²）/ 批量回归 / 调优问答，四入口 | 四入口共用同一 SKILL 集与数据契约（P4） |
| L3 Agent 编排层 | OpsTuningAgent 五阶段主循环 + perf-implementer 单机制实施 | 只编排不判定（P5）：消费 verdict.json，不做 Bound 判定、不写代码 |
| L2 SKILL 能力层 | ops-collect / ops-verdict（★核心增量）/ ops-mechanism / ops-family | 确定性沉淀、可独立使用；判定只出自脚本（D1） |
| L1 工具层 | msprof / msprof op / msopprof / cannsim / CANNBench / npu-smi / acl | 复用不重做（P3）；CANN 知识源经 K5 引用层动态引用不落副本（D9） |

## 4.2 数据流主线

一次调优的全部交付物沿这条链流动——**这条链上的每个节点都是一个有 schema 的契约文件**（§9）：

```
工程 + cases
    │
    ▼ ops-collect
collection_manifest.json ──── collection_manifest（采集块状态机真源）
    │
    ▼ ops-verdict verdict.py
verdict.json ──────────────── 判定书（findings + 反证 + 置信度 + speedup_range + mechanism_refs）
    │                                   │
    ▼ snapshot.py                        ▼ mechanism_refs 路由
baseline/result.json ── 口径指纹  ops-mechanism + ops-family
    │                                   │
    │                                   ▼
    │                        candidates.json（≤3 张方案卡）
    │                                   │
    ▼ perf-implementer 实施（单机制） ◀──┘
round-N/result.json ◀── snapshot.py（主 agent 统一采集）
    │
    ▼ delta_compare.py（口径校验 + 精度门禁 + 噪声带三段结论）
comparison.md ─────────────── KEEP / REVERT / 复测
    │
    ▼ 汇总
REPORT.md（六要素） + case_candidate.json（E 级回灌） + calibration_data.json（校准数据）
```

## 4.3 数据集模型（八类）

| 类 | 数据 | 来源 | 消费方 |
|----|------|------|--------|
| D1 环境指纹 | SoC / CANN / driver / firmware / 设备 / 频率 / commit | npu-smi / version.info | snapshot（HARD_KEYS 子集）、跨环境禁令 |
| D2 算子元数据 | 算子名 / DSL / shape / dtype / format / TilingKey / blockDim / kernel 清单 | 工程文件 / cases | snapshot、L2 判定 |
| D3 执行配置 | warmup / repeat / 计时方法 / baseline 种类 | benchmark / 事件计时 | snapshot（HARD_KEYS） |
| D4 性能计数器 | Task Duration / pipe ratio / active_bw / L2 hit / bank 冲突 | msprof 8 CSV | verdict（pipe/scalar 指标） |
| D5 流水时序 | pipe 泳道 / set-wait 事件 / bubble | trace.json / PipeTimeline | 框架期 overlap_ratio / 串行指纹 |
| D6 源码映射 | 源码行 / PC 热点 / stall | PcSampling / DBI | L0 规则 |
| D7 通信维度 | 各 rank Task Duration / T_comm / T_compute / 同步点 | msprof 多 rank + 隔离测试 | L3 规则（R 值） |
| D8 基线锚点 | t_hw_us / baseline_perf_us / 历史最优 | CANNBench metadata / 基线库 | Phase 1 标尺读数（框架期出口接入） |

> 任一项变化都应重建基线（跨环境比较禁令，由 HARD_KEYS 校验机械化执行）。

---

# 5. 核心设计决策（D1-D10）

## D1 判定进脚本、解释进 LLM（架构第一原则）

**问题**：业界 Bound 判定普遍靠 LLM 阅读文档执行——判定质量随模型/版本波动，阈值变更不可回归，LLM 幻觉无法根除（「无证据宣称 Bound」被多份调优规范明令禁止，恰说明它常发生）。

**决策**：判定 = `ops-verdict` 规则引擎（声明式 YAML 规则 + 脚本求值）。LLM 只做三件事：

1. **编排**（Agent 层）：调度、门禁、汇报；
2. **解释**（消费 verdict.json）：把 finding 的 evidence/counter_evidence 翻译给人听；
3. **机制选择**（消费机制卡）：在 ≤3 张候选方案卡中决策。

**效果**：判定书每条 finding 带证据引用（指标值 + 阈值来源）——LLM 幻觉在判定环节被架构性根除。

**边界**：LLM 不得输出规则库之外的判定；观察到规则未覆盖的形态只能作为假设并标注缺失指标（INSUFFICIENT_DATA 语义），不得写成结论。

## D2 规则-指标绑定契约（判定质量的机制保障）

**问题**：「指标没采却报出瓶颈」是判定误报的主要来源之一；纪律性约束（文档写「无证据不得判定」）无法机械化执行。

**决策**：每条规则声明两级指标绑定：

- `metrics_required`：缺失 → 规则 **SKIPPED**（不触发、不误报、留补采提示）；
- `metrics_optional`：缺失 → 降级评估（反证条目标 `unevaluated`，不阻断主判定）。

对标 NCU「规则声明绑定 section，未采集则规则不出现」——把「无证据不判定」从纪律变成**机制**。

## D3 分层瀑布 + 路由副作用（瓶颈污染的架构解）

**问题**：MC² 场景 MTE2 ratio 被通信等待污染，单核访存优化做了白做。该检查若散在文档靠 LLM 每次阅读执行，存在漏执行风险。

**决策**：判定按 **L3 卡间 → L2 核间 → L1 单核 → L0 指令** 依序执行，且允许**路由副作用**：

```
L3_MTE2_POLLUTED 命中（ratio_full−ratio_pure > 10pp 或 pure/full < 80%）
    │
    ├─ 禁用 L1_MTE2_BOUND（跨规则包传播）
    └─ 改道机制路由 → mc2.md 通信掩盖策略
```

**配套语义——条件三值逻辑**：条件求值结果 ∈ {True, False, **None**}；None = 证据不足 → 规则置 INSUFFICIENT_DATA 并提示补采，**绝不因数据缺失而判「无 bound」**（None 不冒充 False）。这是对「全 pipe 低占用不是无 bound」判读经验的机制化延伸。

## D4 口径指纹互锁（同口径铁律的数据结构化）

**问题**：跨口径比较（换设备/换 shape/换计时方法）使性能数字无意义；噪声带内的波动被当成收益宣称；精度失败的轮次数字混进报告。

**决策**：口径指纹互锁（测量指纹 ↔ 对比硬校验）设计，收敛为两个脚本：

- `snapshot.py`：每次性能测量落**口径指纹**（HARD_KEYS 12 字段 + MECHANISM_KEYS 2 字段 + 精度/误差指纹 + status 三态 ok/partial/dry_run）；
- `delta_compare.py`：三道硬校验（任一失败 exit 2 拒绝生成对比）——
  1. **口径硬校验**：HARD_KEYS 任一不一致 → 拒绝比较（跨口径比较禁令机械化）；
  2. **精度门禁**（第三道，脚本化）：任一侧 precision ≠ pass → 拒绝；
  3. **噪声带三段结论**：speedup > 1.05 → KEEP；< 0.95 → REVERT（回滚后须复跑确认回基线水平）；带内 → 不判收益不判劣化 → repeat 翻倍复测。

**语义演进一并继承**：`tiling_key` / `block_dim` 是分核/tiling 类优化**本身就会改变**的变量，从「禁止比较」降级为「WARN 并写入报告」（MECHANISM_KEYS）——这是对优化本质的正确建模。

## D5 五阶段主循环 + 单变量铁律（流程骨架）

**问题**：流程纪律散在长文档与插件 prompt，无统一骨架；负优化处置、停止条件依赖执行者自觉。

**决策**：以完整流程闭环为蓝本（多轮机制与全量 case 约束一并吸收），收敛为五阶段（详见 §6.2）。三条铁律全程生效：

1. **精度三道门禁**：进入（Phase 0 全集）/ 每轮（实施后先精度后性能）/ 结论（delta_compare 脚本化）；
2. **单变量归因 + 负优化四机制**：可预防（候选须有当前算子 profiling 证据）/ 可检测（同口径 + 噪声带 + 冷稳态区分）/ 可回滚（diff + 备份 + 回滚后复跑验证）/ 可交代（回滚记录进报告——负优化记录是瓶颈证据的组成部分，不是失败；实测负优化率约 1/3）；
3. **停止条件不硬编码轮数**：达标 / 候选尽 / 连续两轮无稳定收益 / 接近上限（NEAR_LIMIT）。

## D6 知识按消费时机分层（渐进式披露）

**问题**：批量加载 references 既爆上下文预算又诱导「先看机制猜瓶颈」（证据先行的反面）。

**决策**：知识加载严格绑定流程阶段：

```
采集前   tool-routing.md（工具选型决策树）
判定后   机制卡（findings[].mechanism_refs 精确路由）——判定之前禁止加载机制/案例
实施时   族 guide + 模板链路（模板拷贝纪律 T1-T5）
归档时   案例卡 schema + 证据分级 + 降级策略
```

配套三条知识工程纪律：①SKILL.md 只做路由与触发，正文进 references；②每文档头部声明适用边界（soc/模式/形态）；③「知识库暂未收录」显式标注并计数（缺口感知回路的输入）。

## D7 职责隔离的编排（继承已验证的设计）

**问题**：实施者自采性能 → 口径漂移；分析者既分析又改码 → 归因失效。

**决策**：四条经实践验证的职责隔离，并扩展：

| 隔离 | 设计 | 来源 |
|------|------|------|
| 分析者不判定 | 判定出自 verdict.py，Agent 只消费 | 本架构扩展（D1） |
| 实施者单机制 | perf-implementer 一次领一张方案卡（可归因） | 工程实践验证 |
| **主 agent 垄断统一采集与 delta_compare** | 各机制若自采则口径漂移 | 工程实践验证 |
| 模板优先双重落地 | 知识层（ops-family T1-T5）+ 编排层（implementer C2-C6）同一指令 | 工程实践验证 |

## D8 执行式评测（判定质量的回归保障）

**问题**：评测普遍全 text——判定知识变更后，无人知道判定质量是否劣化。

**决策**：判定进脚本后评测跟着进脚本：`evals/run_evals.py` 用 fixture（已知答案的 CSV/标量输入，源自真机调优案例）断言 verdict 输出，覆盖正例/反例/边界/绑定契约/路由副作用。**退出码 1 = 阻断合入**（CI 语义）——这是对「判定与评测」双短板的针对性补齐，也是阈值纪律（§10.3）的执行机制。

## D9 外部知识动态引用（应对芯片/API 演进的更新机制）

**问题**：芯片代际演进（A2→A3→A5→下一代）与 AscendC API 演进（新增 MicroAPI/Blaze/
SIMT/BufferID，列名三代 aic_/aiv_/ai_ 兼容）会让任何「落在本仓的事实性知识副本」持续腐烂
——每代新芯片发布都要人工同步一批参数、API 签名、示例代码，同步滞后即判定与实施出错。

**决策**：知识按「**是否随芯片/API 演进**」分两类，只有第二类自建自维护：

| 类别 | 内容 | 归属与更新方式 |
|------|------|--------------|
| **事实性知识（外部引用，不落副本）** | 芯片参数（核数/Buffer 容量/频率/带宽）、API 签名与约束、官方示例代码、编译入口、理论标尺（t_hw/baseline） | 引用 CANN 生态真源：`$ASC_DEVKIT_DIR/docs/api` 与 `examples`、cann-bench `tasks/metadata/*.json`、官方算子仓；**运行时解析引用**，本仓只存「引用清单」（路径/commit 锁定），不复制内容 |
| **经验性知识（本仓自建，版本化沉淀）** | 判定规则与阈值、机制卡、案例卡、模板货架、口径契约 | 不随 API 演进而失效（绑定的是「指标→瓶颈→机制」因果，不是 API 签名）；随实测回流持续进化（四回路） |

引用层机制（K5，见 §10.5）：

1. **引用清单 + 版本锁定**：`adapters/` 下声明外部真源路径与锁定版本（cann-bench metadata
   的 `VERSION`、devkit 版本号）；升级 = 换引用 + 过回归，不是改内容；
2. **运行时探测优先**：能探测的参数一律运行时获取（`GetCurNpuArch()` / `GetCoreNumA*` /
   `GetCoreMemSize` / 本机 `--help`），探测不到才查引用文档——与「禁止硬编码芯片参数」
   的既有纪律（npu-arch 真值表原则）同构；
3. **演进隔离**：芯片/API 变化首先体现为「外部知识源版本 bump」，本仓受影响的只有适配层
   （列名兼容、soc 映射）与规则包 soc 域——**演进面收敛在适配层，不散布在知识正文**；
4. **失效检测**：CI 周期任务校验引用可达性（devkit 路径存在、metadata schema 未变），
   引用失效 = 用例红，而不是判定悄悄出错。

**边界**：模板货架（ops-family）中抄自官方示例的代码保留原许可证头并标注来源 commit——
「拷贝纪律」不变，但模板的**更新策略**是追上游引用（上游变了→重新评估拷贝版本），
不是本仓独立演进一份分叉。

## D10 场景路由机制（五级路由瀑布）

**问题**：四类用户场景 × 卡间/核间/单核瓶颈层次 × 十个算子族 × 三代芯片，如果 Agent 用
一个扁平流程「全都跑一遍」，简单算子被过度流程、融合算子被漏检（MTE2 污染前置检查一旦
漏执行即误判）、无卡用户被直接卡死。路由不是便利功能，是判定正确性的前提（D3 路由副作用
依赖 L3 先于 L1 执行）。

**决策**：五级路由瀑布，每级一个路由轴、一个决策载体（脚本/表格，不靠 LLM 记忆）：

```
R1 场景路由（agent/AGENTS.md 场景表）
   输入特征 → 单算子 | 融合算子(FA/MC²) | 批量回归 | 问答
   裁剪流程：全五阶段 / 污染前置+L3全启用 / 基线直取+回归门禁 / 仅Phase 2
        │
R2 采集模式路由（ops-collect 路由决策树，tool-routing.md）
   MC²·fork多rank → task-based 强制 + 隔离测试
   单卡 → msprof op（本机 --help 可用）否则 standard 7 组
   无卡/指令级 → cannsim
        │
R3 判定层路由（ops-verdict 规则包分层瀑布）
   L3 卡间 → L2 核间 → L1 单核 → L0 指令；上层结论约束下层（路由副作用）
   纯单算子：L2+L1[+L0]；融合算子：L3 全启用（隔离测试 → R 值 → 瀑布）
        │
R4 机制路由（verdict findings[].mechanism_refs → 机制卡，渐进式披露）
   判定后按 rule_id 精确路由到机制卡（≤3 张候选方案卡）；污染命中自动改道
        │
R5 族路由（ops-mechanism 方案卡 template → ops-family 模板货架）
   族分类表 → 族 guide → 模板表 → .h/.cpp/.template；融合算子拆基元族
```

| 级 | 路由轴 | 决策载体 | 错路由的后果 |
|----|--------|---------|-------------|
| R1 | 调优场景 | AGENTS.md 场景表（4 行） | MC² 走单算子流程 → 污染未检 → 单核误判 |
| R2 | 采集模式 | tool-routing 决策树（3 硬规则） | fork 程序走 msprof op → 数据不可靠 |
| R3 | 判定层次 | 规则包 YAML（layer/priority/soc 域） | 层序颠倒 → 路由副作用失效 |
| R4 | 优化机制 | mechanism_refs（判定→机制契约） | 证据先行被破坏（先看机制猜瓶颈） |
| R5 | 算子族 | 族分类表 + 模板链路 | 选错实现（broadcast 三实现选错直接慢/nan） |

**设计要点**：① 每级路由的决策依据都是**结构化产物**（场景表/决策树/YAML/契约字段），
LLM 只执行查表不做发明——「路由进表格，不进记忆」；② 路由之间显式衔接（R1 决定 R2 用
哪个分支，R2 产出决定 R3 输入，R3 findings 决定 R4 加载哪些卡，R4 的 template 决定 R5），
上游路由错误会在下游契约校验处暴露（如非融合场景出现 L3 finding）；③ 新增路由维度
（新算子族/新 DSL/新芯片）= 在对应级加表项，不改路由骨架。


---

# 6. Agent 层设计

## 6.1 OpsTuningAgent（primary）

**职责四件**：

1. **意图与边界**：理解调优目标（达标线/加速比/找瓶颈）；场景分类（§6.3）；精度门禁预检（工程正确性全集跑通，失败 → 拒绝进入调优）；
2. **工作流调度**：按阶段调 subagent；读 `tuning_artifacts/` 产物判进度、驱动门禁（Step 完成判定见 task-prompts.md）；
3. **进度汇报**：每轮三段结论；多轮汇总（各轮改进幅度 + 相对首轮基线总改进）；
4. **争议仲裁**：机制选择/判定解释分歧 → 查机制卡与官方文档 → 裁决追加 REPORT.md `## 仲裁记录`；优先级：官方文档 > 机制卡实测 > 工程可行性。

**禁做清单**（纪律）：直接参与判定/策略制订/代码实施；跳过基线直接分析；跳过判定直接选机制；凭经验给优化建议；改写 subagent prompt；一次迭代多个无关机制；噪声带内宣称收益。

**输入**：可运行工程 + cases（>20 取前 20 并告知截断）+ 输出目录 +（可选）性能目标/max_rounds。
**输出**：scope.json → verdict.json → candidates.json → 逐轮 result/comparison → REPORT.md → 回灌条目。

## 6.2 五阶段主循环

```
Phase 0 意图与边界（本 agent）
    ├─ 精度门禁预检（①）：工程正确性全集跑通？失败 → 拒绝，转调试域
    ├─ 场景分类：单算子 / 融合算子（FA·MC²）/ 批量回归 / 问答
    └─ scope.json 落盘（13+ 字段口径指纹）
        │
Phase 1 环境与基线固化（本 agent + /ops-collect）
    ├─ 环境指纹（environment.txt：npu-smi/CANN/设备/频率/负载/commit）
    ├─ 工具路由（MC² → task-based 强制）
    ├─ 不可变基线：snapshot.py（timing-method 五选一 + HARD_KEYS 12 字段）
    └─ （框架期出口）理论标尺：CANNBench t_hw / 历史最优 →「还差多远」读数
        │
Phase 2 采集与分层判定（/ops-collect + /ops-verdict）
    ├─ 标准 7 组指标（或 MC²：隔离测试 → R 值 → 全瀑布）
    ├─ verdict.py：L3→L2→L1→L0；INSUFFICIENT_DATA → 按缺项补采（≤2 轮）
    └─ 路由副作用生效（污染 → 禁用单核访存判定 + 改道通信掩盖）
        │
Phase 3 机制路由与候选方案（/ops-mechanism + /ops-family）
    ├─ 按 mechanism_refs 加载机制卡（渐进式披露：判定后才加载）
    ├─ 模板路径进方案卡（✅可直接拷贝 / ⚠️部分实现 / ❌仅设计参考）
    └─ candidates.json（≤3 张方案卡，target_rule 证据绑定 + rollback_if 非空强制）
        │
Phase 4 单变量迭代（每轮；perf-implementer + 本 agent）
    ├─ 4a 并行分发：每方案卡一个 implementer（复制目录→模板拷贝→编译修复→全量精度）
    ├─ 4b 主 agent 统一采集（同口径铁律）→ snapshot → delta_compare
    │    ├─ KEEP → 保留，产物为新基线，进入下一轮
    │    ├─ REVERT → 回滚并复跑确认回基线水平；负优化记录进报告
    │    └─ 噪声带内 → repeat 翻倍复测；仍带内 → 按无稳定收益处置
    ├─ 机制生效证据复采：verdict 复跑（期望 target_rule 转 NOT_HIT 或 severity 降档）
    └─ 停止条件：达标 / 候选尽 / 连续两轮无稳定收益 / NEAR_LIMIT（不硬编码轮数）
        │
Phase 5 对比归档与回灌（本 agent）
    ├─ REPORT.md 六要素（见下）
    ├─ case_candidate.json（E 级回灌）+ calibration_data.json（预估-实测对照）
    └─ 多轮场景：各轮改进幅度 + 相对首轮基线总改进
```

**REPORT.md 六要素（缺一即不合格）**：① 环境与源码指纹；② 逐 case 对比表（全量，禁只展示最快样本）；③ Bound 与指标变化（前后 verdict 对照）；④ 保留与回滚清单（负优化轮次与原因——回滚记录是瓶颈证据的组成部分）；⑤ 预估 vs 实测（喂校准回路）；⑥ 适用边界标注（partial/dry_run/soc 域）。

## 6.3 场景路由（四入口裁剪）

| 场景 | 入口特征 | 流程裁剪 | 关键差异 |
|------|---------|---------|---------|
| 单算子 | 可运行工程 + cases | 全五阶段 | 逐 case 全量覆盖（禁止代表性用例） |
| 融合算子 | FA/MC² 识别（fork 多 rank / CrossCore / AIC+AIV 绑核） | Phase 2 前置污染检查 + L3 全启用 | 隔离测试编排、R 值路由、跨 rank max、msprof 强制 |
| 批量回归 | 基线库已有历史 | Phase 1 简化（基线直取）+ Phase 3-4 改重跑判定+对比 | 回归门禁语义（阈值告警），不做机制迭代 |
| 问答 | 已有 profiling 产物 | 仅 Phase 2 离线判定 + 报告解读 | 禁止无证据输出 Top5；数据不足按真实数量输出，不补齐不造数 |

## 6.4 perf-implementer（subagent）

单机制实施：理解方案卡（证据不匹配 → 返回主 agent 不硬做）→ 模板拷贝优先（T1-T5）→ 编译修复顺序（禁直接降级）→ 语义变更类附边界用例 → Release 编译（禁 -O0）→ 全量 case 精度（工程现有容差，禁自创）→ 返回「代码目录 + 编译状态 + 逐 case 精度明细 + 模板使用情况 + diff 摘要」。

**核心约束**：一次一个机制（单变量）；不采集性能、不生成对比（主 agent 垄断）；精度全过才返回；tiling 改动后 device fault 处置四步（回滚确认基线可复跑 → -g 重编 + msSanitizer 定位 → 缩小改动粒度 baseK 128→192 非 256 → 核对深度/容量联动字段）；遇精度/崩溃/hang → 立即转调试域。

## 6.5 门禁时序（一次完整调优的门禁链）

```
进入门禁           每轮门禁                结论门禁（脚本化）
Phase 0 ────────▶ Phase 4 实施后 ────────▶ delta_compare
精度全集跑通         先精度后性能             precision≠pass → exit 2
失败→转调试域        fail→回滚+数字不进报告    口径漂移 → exit 2
                                           噪声带 → 复测不宣称
```

**多轮机制**：默认 1 轮；用户指定 `max_rounds=N`（≥2）最多 N 轮；指定 `performance_goal` 默认最多 2 轮（第 1 轮达标即停）；多轮显式 opt-in，agent 不自行进入下一轮；第 N+1 轮基线 = 第 N 轮 KEEP 产物；每轮必须重新走 Phase 2（判定不复用）。

**Step 门禁与调用模板**：主 agent 调用任何 subagent 前必须先读 `workflows/task-prompts.md` 对应 Step 完整模板，禁止凭 AGENTS.md 概述自行构造 prompt、禁止自行编写/删减/改写模板内容（只允许替换 `{operator}` 等占位符）。

## 6.6 与算子开发工作流的关系

本 Agent 是**调优域独立编排**（插件形态：AGENTS.md primary + agents/ subagent + workflows/ task-prompts）。算子**开发**走开发域工作流（如 CANN 算子开发流程），两域通过产物目录衔接：

```
开发域工作流的精度验收（operators/{op}/docs/precision/）
    ──▶ 本 Agent Phase 0 精度门禁的输入（入口前提：精度通过）
本 Agent REPORT.md
    ──▶ 归档回 operators/{op}/docs/（调优域产物与开发域产物同处一仓）
```

---

# 7. 判定引擎详细设计

## 7.1 设计目标

| 目标 | 度量 |
|------|------|
| 判定可审计 | 每条 finding 带证据数值 + 阈值来源（threshold_source）+ 反证评估 |
| 判定可回归 | 规则/阈值变更 → evals 全绿才可合入 |
| 判定可扩展 | 新规则 = 一个 YAML 条目 + fixture + 断言（不动引擎代码） |
| 判定跨芯片 | 规则包 soc 域硬过滤；列名三代兼容（aic_/aiv_/ai_） |
| 无证据不判定 | REQUIRED 缺失 → SKIPPED；数据不足 → INSUFFICIENT_DATA |

## 7.2 规则包结构（opstune.rules/1.0）

```yaml
schema: opstune.rules/1.0     # 固定
layer: L1                     # L3 卡间 / L2 核间 / L1 单核 / L0 指令
title: 单核流水判定（核内 pipe 级）
soc: [a2, a3, a5]             # 芯片域硬过滤：不含目标 soc 则整包跳过
requires_experiment:          # （可选）实验数据前置
  kind: isolation_test        #   如 MC² 隔离测试；缺失时本包规则整体不触发
  outputs: [t_comm_us, t_compute_us, m_rows]

metrics:                      # 指标词汇表（绑定契约的解析层；规则只能引用已声明指标）
  mte2_ratio:
    source: pipe              #   pipe（CSV 逐行聚合）/ scalar（调用方提供）/ derived（引擎计算）
    columns: [aic_mte2_ratio, aiv_mte2_ratio, ai_mte2_ratio, "Mte2Utilization(%)", ...]
    normalize: ratio_to_pct   #   0~1.5 视为比例 ×100（双版本字段口径兼容）
    group: pipe               #   参与主导 pipe 排序与串行指纹求和
  mte2_active_bw_pct_of_peak:
    source: scalar            #   由 --scalar 提供（带宽占峰值百分比等）

rules:
  - id: L1_MTE2_BOUND                # 全局唯一（跨包路由引用）
    name: MTE2 搬入瓶颈
    priority: 10                     # 升序求值（小者先评估）
    parent: L1_MEMORY_SATURATION     # DAG 父节点（权重传播自进化期启用）
    threshold_source: "[msprof-doc] >80% 直接判 / 最大且 >70%"
    metrics_required: [mte2_ratio]           # 绑定契约：缺失 → SKIPPED
    metrics_optional: [mte2_active_bw_pct_of_peak, fixpipe_ratio]  # 缺失 → 降级
    condition:                       # 触发条件（三值逻辑）
      any:
        - {metric: mte2_ratio, agg: mean, op: gt, value: 80}
        - all:
            - {metric: mte2_ratio, agg: mean, op: gt, value: 70}
            - {metric: mte2_ratio, agg: rank, op: eq, value: 1}   # rank=1：主导 pipe
    watch:                           # 观察档（未达判定线时提示，不判 bound）
      condition: {metric: mte2_ratio, agg: mean, op: gt, value: 50}
      note: MTE2 占比 50~70%，暂非主导，已具备搬运优化价值
    severity:                        # 分档：critical / moderate / light
      metric: mte2_ratio
      bands: [{min: 85, level: critical}, {min: 70, level: moderate}]
      default: light
    counter_evidence:                # 反证条目（防误判的第二道闸）
      - id: true-bandwidth-saturation
        condition: {metric: mte2_active_bw_pct_of_peak, agg: value, op: gt, value: 80}
        note: active_bw 已达理论 80%+：真带宽饱和。优化在数据量/复用，不在搬运组织
        action: annotate             # annotate（记录）/ reroute（改道，框架期）
    confidence: high                 # 规则级先验：high / medium / low
    speedup_range: [1.2, 2.0]        # 预估加速比区间（校准闭环回写）
    speedup_source: "[tuning-practice] MTE2 优化方法预期收益，🟡 首期标定值"
    mechanism_refs:                  # 机制路由入口（判定→机制的契约）
      - ops-mechanism:references/optimize/data-copy.md
    routing:                         # （可选）命中副作用
      on_hit: [disable_rule, L1_MTE2_BOUND]   # 禁用指定规则（跨包生效）
      reroute_to: ops-mechanism:references/optimize/mc2.md
```

## 7.3 求值语义

### 7.3.1 指标聚合

| 环节 | 语义 | 来源与理由 |
|------|------|-----------|
| pipe 指标 | 逐行取候选列（按序首个可解析者）→ per-row 取 aic_/aiv_ **双列最大值** → 跨行 mean/max 双统计 | A5 真机实测（vector kernel 有效数据在 aiv_* 列，aic_* 常为 NA，反之亦然） |
| 判定口径 | **判定用 mean（核间平均），max 仅识别长尾** | 「单核峰值不能代表全 kernel」实测纪律 |
| 字段口径 | ratio_to_pct：0 ≤ v ≤ 1.5 视为比例 ×100 | 双版本字段口径实测 |
| 主导 pipe | `agg: rank, op: eq, value: 1` | 「最大且 >70%」判定表 |
| 串行指纹 | `__pipe_sum_mean ∈ [85,115]`（多 pipe 互补求和≈100%） | 真机案例（cube 0.44+mte2 0.42+mte1 0.18≈1 → 单缓冲+逐级同步） |
| 逐核长尾 | max > mean × 1.25 | 逐核耗时实测 |
| 离散度三档 | (max−min)/max：<10 达标 / 10-30 警告 / >30 严重 | 逐核负载实测标定 |
| 两簇检测 | 前后半段核均值差 ≥2% → NUMA/L2 slice 偏斜提示 | 逐核负载实测 |
| 头开销 | Task Duration − max(aic,aiv) 占比 >30% | 实测（A2 满核头开销 ~20-21us 参考值） |
| R 值 | R = T_compute / T_comm 五档 | MC² 多卡实测（R 跨 0.3~9.8） |
| MTE2 污染 | full−pure > 10 个百分点 或 pure/full < 80% | MC² 隔离测试实测（比例口径统一为百分数） |

### 7.3.2 条件三值逻辑

| 节点 | True | False | None（证据不足） |
|------|------|-------|----------------|
| `all: [...]` | 全部 True | 任一 False | 有 None 无 False |
| `any: [...]` | 任一 True | 全部 False | 有 None 无 True |
| 叶子 | 条件成立 | 条件不成立 | 指标缺失/无法比较 |

- **None 不冒充 False**：无法评估 → INSUFFICIENT_DATA + 补采提示，绝不判「无 bound」；
- `value` 支持复合引用：`{metric: time_mean_us, factor: 1.25}`（另一指标 × 系数，用于长尾类比较）；
- `op` 集合：`gt / gte / lt / lte / eq / between [lo,hi] / gt_ratio / lt_ratio`。

### 7.3.3 规则状态机

```
                 ┌─ REQUIRED 缺失 ──────▶ SKIPPED（绑定契约；missing 列表供补采）
                 │
规则（按 priority）─┼─ 条件求值 None ──────▶ INSUFFICIENT_DATA（缺 OPTIONAL 指标提示）
                 │
                 ├─ 条件 False ──────────▶ NOT_HIT（watch 档单独提示）
                 │
                 └─ 条件 True ───────────▶ HIT：severity 分档 + counter_evidence 评估
                                            + confidence + speedup_range + mechanism_refs
                                            + routing 副作用（disable/reroute，跨包传播）
```

### 7.3.4 汇总判定（verdict 三态）

| verdict | 触发 | 语义 |
|---------|------|------|
| `NEEDS_OPTIMIZATION` | 有 HIT | 按 severity × confidence 排序输出 headline + Top3 机制路由 |
| `NEAR_LIMIT` | 无 HIT 且有被完整评估过的规则（NOT_HIT） | 各 pipe 均衡低于判定线——评估是否已达目标 |
| `INSUFFICIENT_DATA` | 无 HIT 且零完整评估 | 全部规则因指标缺失跳过——**禁止**在此状态下输出任何判定结论 |

特例：唯一 critical + high 置信 + speedup 上界 < 1.15 → NEAR_LIMIT 提示（「确认标尺后再投入」）。

## 7.4 首期规则清单（23 条，M1）

| 层 | 规则 | 阈值（含来源） |
|----|------|--------------|
| **L1 单核**（8） | MTE2 / CUBE / VEC / MTE3 / FIXP / SCALAR BOUND | >80% 直接判 / 最大且 >70%；VEC 三档 50-65/65-80/>80；SCALAR watch 30%；FIXP watch 15%（对齐预警） |
| | LATENCY/SYNC 可疑 | 全 pipe mean <50%（「不是无 bound」判读） |
| | 串行无重叠指纹 | max pipe <70% 且 pipe 求和 85~115% |
| **L2 核间**（5） | 核间长尾 | max > mean×1.25（critical ≥1.6） |
| | 离散度警告/严重 | (max−min)/max：10~30% / >30% |
| | 切核不足 | 活跃核 < 物理核×33%（A5 约 48 AIV / 28 AIC） |
| | 头开销占比 | (Task−max(aic,aiv))/Task >30% |
| **L3 卡间**（7） | MC² 平衡/计算/强计算/通信/强通信 | R∈[0.9,1.1] / (1.1,2] / >2 / [0.5,1) / <0.5 |
| | MTE2 污染（路由规则） | full−pure >10pp 或 pure/full <80% → 禁用 L1_MTE2_BOUND |
| | 跨 rank 不均衡 | max/mean >1.1 🟡 |
| **L0 指令**（3） | ICache miss / L2 hit / stall 主因 | >15% / <50% / >40%（stall 为框架期占位） |

每条规则的反证条目（MTE2 真饱和/假 bound、bank conflict 放大、Cast 重、NUMA 两簇、小 case 减核权衡等）与全量阈值溯源见 `ops-verdict/references/thresholds.md`（相对本套件根目录）。

框架期扩展方向：FA 混合绑核档（CrossCore flag 粒度 / AIC:AIV 配比）、SIMT/VF 档（divergence/spill/bank conflict/warp sync）、通信带宽档。自进化期扩展：父子 DAG 权重传播（GLOBAL/LOCAL 加速比语义）。

## 7.5 执行式评测体系（evals/run_evals.py）

10 条用例 = 10 个已知答案的调优场景（fixture 输入 → 断言输出）：

| # | 用例 | 断言要点 |
|---|------|---------|
| E1 | MTE2 占比 78% 且最大 pipe | L1_MTE2_BOUND 命中、VEC/CUBE/LATENCY 不命中、NEEDS_OPTIMIZATION |
| E2 | E1 + active_bw=85% 峰值 | 命中**反证**「真带宽饱和」（优化方向改变仍 HIT） |
| E3 | cube 0.44+mte2 0.42+mte1 0.18≈1 | 串行指纹命中、CUBE/MTE2 BOUND 不命中 |
| E4 | 逐核耗时均值 93us 峰值 185us | 核间长尾 + 离散度严重双命中 |
| E5 | 仅 8 个 block 行（A5 48 核） | 切核不足命中 |
| E6 | T_comm=420 T_compute=880 → R=2.10 | 强计算 bound 命中、平衡/通信不命中 |
| E7 | R=1.01 | 完美平衡命中 |
| E8 | ratio_full=62 ratio_pure=41 | **污染命中 + 路由副作用生效** |
| E9 | 无 CSV 无标量 | 全部 SKIPPED + **INSUFFICIENT_DATA**（绑定契约兜底） |
| E10 | 全 pipe <50% | LATENCY/SYNC 可疑命中而非「无 bound」 |

**纪律**：新增/修改规则必须同时提交 fixture 与断言（正例 + 至少一个反例/边界）；`python3 evals/run_evals.py` 全绿才可合入。

## 7.6 使用示例（真实命令与输出）

```bash
# ① 全瀑布判定（MC² 算子）：L3 隔离测试标量 + L2/L1 CSV
python3 ops-verdict/scripts/verdict.py \
    --rules ops-verdict/rules/L3-inter-card.yaml,ops-verdict/rules/L2-inter-core.yaml,ops-verdict/rules/L1-single-core.yaml \
    --csv PipeUtilization.csv --soc a5 \
    --scalar t_comm_us=420,t_compute_us=880,mte2_ratio_full=62,mte2_ratio_pure=41,physical_cores=48 \
    -o verdict.json
```

实测输出（fixture：core_tail_a5.csv + 标量）：

```
verdict: NEEDS_OPTIMIZATION  |  通算强计算 Bound（critical）；另有 5 项命中
  HIT  [moderate/high  ] L3_MTE2_POLLUTED: MTE2 指标被通信等待污染（路由规则）  evidence: mte2_ratio_full=62.0；mte2_ratio_pure=41.0
  HIT  [critical/high  ] L3_MC2_STRONG_COMPUTE: 通算强计算 Bound  (est [1.0, 1.2])  evidence: t_comm_us=420.0；t_compute_us=880.0
  HIT  [critical/high  ] L2_IMBALANCE_SEVERE: 核间负载不均衡（离散度严重档）  (est [1.1, 1.5])  evidence: time_mean_us=139.5；time_max_us=187.3；time_min_us=91.8
  HIT  [moderate/medium] L1_SERIAL_NO_OVERLAP: 串行无重叠指纹  (est [1.3, 2.0])
        ⚠ 反证[serial-fingerprint]: 多 pipe 互补求和≈100%：串行无重叠指纹，Double Buffer / 流水掩盖是首选机制
  skipped=3  insufficient_data=0
```

---

# 8. SKILL 层设计

## 8.1 职责切分总表

| SKILL | 回答的问题 | 输入 → 输出 | 确定性载体 |
|-------|-----------|------------|-----------|
| **ops-collect** | 数据怎么采对采全 | 工程信息 → manifest + CSV + iso_result | collect_board.sh / isolation_test.py / 路由决策树 |
| **ops-verdict** | 瓶颈是什么、为什么不是别的；改好了吗 | CSV/标量 → verdict.json；性能 → result.json；前后 → comparison.md | 规则包 + verdict/snapshot/delta_compare + evals |
| **ops-mechanism** | 怎么改、能涨多少、风险在哪 | findings → 候选方案卡 | 机制卡八要素 + 案例卡 schema + 证据分级 |
| **ops-family** | 代码怎么写（货架） | 族名+机制 → 模板路径 | 族分类表 + A/B 双型文档 + 模板拷贝纪律 |

## 8.2 ops-collect：采集编排

**采集模式路由**（三条硬规则）：

```
① 先探测本机帮助，再选命令（所有可选参数以本机 --help 为门禁；
   禁止「MC² 永远不能用 msprof op」这类跨版本绝对规则——新版支持时可用）
② 已有 collection_manifest.json 的目录禁止重复采集（只允许重解析或换新目录）
③ MC² 禁 host 侧计时；一切性能数据出自 msprof op_summary 的 Task Duration(us)
```

路由决策树：MC²/fork 多 rank → `--mode mc2`（task-based 强制）+ 隔离测试；单卡 → 本机 msprof op 可用则用（能力按帮助）否则 `--mode standard`（7 组 aic-metrics）；无卡/指令级 → cannsim（仅 A5 单卡 AI Core）。

**MC² 隔离测试编排**（isolation_test.py）——把此前长期停留在文档形态、全靠人工执行的隔离测试（复制源码、注释 process、tileCnt=1、多 rank 采集、算 R 值）变成可重复实验编排：支持 env 开关模式（程序自带 COMM_ONLY/COMPUTE_ONLY）与 from-prof 模式（Agent 已在副本完成注释的两份 profiling 目录直接收敛）；稳态纪律与跨 rank max 口径内置。

## 8.3 ops-verdict：判定引擎

见 §7。补充 SKILL 层职责边界：不做采集（ops-collect）、不写代码（Agent 调 implementer）、不做机制解释细节（ops-mechanism）、不渲染报告（报告消费 verdict.json）。**阈值纪律**：修改规则/阈值 → evals 全绿 → 阈值带来源标注 → 合入。

## 8.4 ops-mechanism：机制与案例

**机制卡固定八要素**：原理（硬件层因果链）/ 适用前提（rule_id + 指标窗口 + 容量约束）/ 预期收益（speedup_range + 可行性系数）/ 架构限制（soc 域）/ 精度风险（语义保持 vs 变更分级）/ 验证命令 / 回滚条件 / 模板资产（对接 ops-family 或「无货架」标注）。

**机制卡索引首期 7 张**：pipeline（Double Buffer 全形态 + workspace 份数 + AIC:AIV 配比）/ data-copy（**真假 bound 二分**：真饱和→数据量/复用，假 bound→对齐/合并小搬运）/ memory-hierarchy / api-usage（标量削减/Cast 削减）/ ascendc-tiling / ascendc-regbase-vf（A5）/ mc2 + mc2-balancing（Local Matmul 首选 + 四剪枝 Top-N 配平搜索）。

**案例证据分级**：

| 等级 | 来源 | 可写入 | 禁止 |
|------|------|--------|------|
| A | 官方已合并 PR/MR 含测试表 | 机制/平台/原文数字/精度 | 收益泛化到全场景 |
| B | 官方 PR/MR 无数字 | 代码机制/路径/分支 | 推算或补写加速比 |
| C | 官方 Wiki/指南/sample | 流程/可运行案例/原文数字 | 教程数字当目标峰值 |
| D | 社区/二手 | 仅线索与背景，明确警示 | 作为唯一验收证据 |
| **E** | **本工作流实测回灌** | 机制可信度等同 A；speedup 绑定口径指纹 | 外推 |

配套纪律：检索最多 3 个同型案例；PR 未公开数字统一写「原文未附」禁止从标题推导；**反例与收益案例同等收录**（Triton SIMT→SIMD 24.5x 后被 revert 等）；无同型案例（MoE routing / FA 类混合绑核 / RMSNorm+RoPE 融合等已知缺口）走降级策略——证据链 + 机制卡驱动、实施细节自行推导并提高验证强度、报告标注「无同型案例」、本次证据沉淀为 E 级候选。

## 8.5 ops-family：算子族与模板货架

**族分类表**（10 族 + 3 规划中）：matmul（六策略互斥判定树：pingpong/swat/streamk/fullload/scale_coalescing/mte2_preload，策略-判定规则映射进 guide）/ mc2（3 策略）/ reduce（5 个 Softmax 模板）/ elementwise / broadcast（B 型选型：OneDim 快路径 + NDDMA/DataCopyPad/UB 三实现，选错直接慢甚至 nan）/ conversion / sort / scalar / simt / **fa（已知缺口——降级策略执行）**。

**模板优先纪律 T1-T5**（知识层与编排层双重落地）：

1. 查到模板（.h/.cpp/.template）→ **直接拷贝到项目 op_kernel/**（保留 dav310 等子目录结构），仅做最小适配；.template 先按 usage_guide 转换；
2. 模板内 MicroAPI VF 计算（RegTensor/MaskReg/LoadAsFp32/StoreFromFp32）**原样保留**，禁止降级为高层 API；
3. MicroAPI 编译报错修复顺序：命名空间别名（`namespace MicroAPI = Reg;`）→ API 签名适配 → include 路径 → 全部无效才降级对应分支并留痕（尝试记录 + 失败原因）；
4. 实施方返回「模板使用情况」。

**文档结构 A/B 双型**：A 型单实现调参（优化目标量化收益 → 架构概览 → 关键参数 → 核心循环前后对照 → 关键修改点表）；B 型多实现选型（总览速查 + **选型决策树** + 每实现分册 + code/ 可裁剪源码；收益 = 选对避免劣化，必须给选型依据）。

## 8.6 SKILL 间契约（唯一耦合点）

```
ops-collect ──manifest blocks──▶ ops-verdict（CSV 输入定位）
ops-verdict ──findings[].mechanism_refs──▶ ops-mechanism（机制卡精确路由）
ops-mechanism ──方案卡 template 字段──▶ ops-family（模板路径）
ops-verdict ──snapshot HARD_KEYS──▶ delta_compare（校验域互锁）
```

契约字段不变则两侧互不影响——扩展面集中在各自 references/。

---

# 9. 数据契约体系

七个契约文件构成全部交付物的数据底座——**单一数据源原则**：REPORT.md（人读）、Agent 概览（对话）、CI 检查（JSON 断言）从同一批契约渲染，不存在第二套数字。

## 9.1 采集清单 `opstune.collection/1.0`

| 字段 | 说明 |
|------|------|
| `mode` | standard / quick / mc2 |
| `exe` / `args` / `device_id` / `ranks` / `kernel_name` | 采集口径 |
| `blocks[]` | `{name, status, path, note}`；status ∈ ok/partial/failed/skipped（采集块状态机） |

**纪律**：已有 manifest 的目录**禁止重复采集**（只允许重解析或换新目录）；failed 块在 verdict 侧表现为 REQUIRED 缺失 → SKIPPED（绑定契约兜底）；**禁止伪 Green**（失败块不得从清单悄悄抹掉）。

## 9.2 判定书 `opstune.verdict/1.0`

```json
{
  "schema": "opstune.verdict/1.0",
  "soc": "a5", "rules_files": [...], "csv": "...", "rows": 24, "scalars": {...},
  "facts": {
    "vec_ratio": {"mean": 45.9, "max": 63.0, "rows_present": 24},
    "__pipe_sum_mean": {"mean": 113.5}, "r_value": {"value": 2.1}
  },
  "findings": [{
    "rule_id": "L1_MTE2_BOUND", "status": "HIT", "layer": "L1",
    "severity": "moderate", "confidence": "high",
    "threshold_source": "[msprof-doc] ...",
    "evidence": "mte2_ratio=78.0",
    "counter_evidence": [{"id": "true-bandwidth-saturation", "note": "..."}],
    "speedup_range": [1.2, 2.0], "speedup_source": "🟡 ...",
    "mechanism_refs": ["ops-mechanism:references/optimize/data-copy.md"],
    "routing": {"on_hit": [...], "reroute_to": "..."}
  }],
  "skipped": [{"rule_id": "...", "status": "SKIPPED|NOT_HIT", "missing": [...], "watch": "..."}],
  "verdict": "NEEDS_OPTIMIZATION|NEAR_LIMIT|INSUFFICIENT_DATA",
  "headline": "...", "recommended_next": [...]
}
```

## 9.3 结果指纹 `opstune.snapshot/1.0`

| 字段组 | 字段 | 校验级别 |
|--------|------|---------|
| **HARD_KEYS（口径 12 字段）** | op / soc / mode / timing_method / baseline_kind / cann_version / device_id / shape / dtype / format / warmup / repeat | 不一致 → **禁止比较** |
| **MECHANISM_KEYS（机制变量 2 字段）** | tiling_key / block_dim | 不一致 → WARN 写入报告（`--strict` 恢复禁比） |
| 精度指纹 | precision（pass/fail，必填）/ mismatch / max_abs_err / max_rel_err | precision ≠ pass → 禁止进性能结论 |
| 性能 | kernel_avg_us / timing_method（五选一：event / sim / msprof / msprof_task_duration / profiler_summary） | — |
| 状态 | status ∈ ok / partial / **dry_run**（未上板仅离线结论） | 报告必须标注 |
| 环境 | cann_version / repo_commit / npu-smi 快照 | — |

**计时口径五选一的适用条件**（实测边界情形）：

| 口径 | 适用 | 禁止 |
|------|------|------|
| event | host aclrtEvent / 工程正式 benchmark（默认首选） | — |
| msprof_task_duration | 长 kernel（ms 级）且无 event 设施（报告注明口径） | 短 kernel 用它做基线 |
| msprof | 诊断辅助 | 作最终性能结论 |
| sim | cannsim 仿真 | 与上板数字混比 |
| profiler_summary | 短 kernel（µs 级）wall 不可归因时辅助 | 按 wall 噪声判回滚或宣称收益 |

## 9.4 对比报告 comparison.md

由 delta_compare.py 生成（人读 markdown），核心是三段结论与两道 exit 2 门禁：

```
结论: 提升：speedup=1.533x（>1.05 超出噪声带）        ← 或 劣化/噪声带内
处置: KEEP：该轮保留，进入下一轮单变量迭代             ← 或 REVERT（回滚后复跑）/ 复测
WARN 机制变量变化（预期内）: block_dim: 24 → 48
```

## 9.5 候选方案卡 candidates.json

每轮机制选择输出 ≤3 张（按 speedup_est × 可行性排序）：

| 字段 | 说明 |
|------|------|
| `mechanism` / `target_rule` | 机制名 + 对应判定规则（**证据绑定**：target_rule 必须 ∈ verdict findings） |
| `evidence` / `diff_sketch` | 判定证据摘录 + 改动点描述 |
| `speedup_est` | 预估区间（校准闭环回写实测） |
| `semantic_risk` | preserved / altered（语义变更类必须附边界用例的 precision_plan） |
| `verify_cmd` / `rollback_if` | 验证命令 + 回滚条件（**非空强制**） |
| `template` | 模板路径（ops-family）或 null（无货架标注） |

## 9.6 隔离测试结果 `opstune.isolation/1.0`

`isolation_test.py` 产出：`t_comm_us / t_compute_us / r_value / bound_hint / rank_detail / steady_rule`——直接可喂 `verdict.py --scalar`。

**采集纪律**：隔离测试在源码副本进行；tileCnt=1；循环 ≥10 次前 5 warm-up；**跨 rank 取 max**（木桶效应：所有 rank 经 BarrierAll/CrossCoreFlag 同步，整体性能由最慢 rank 决定）；禁 host 侧计时。

## 9.7 案例回灌条目 case_candidate.json

REPORT 产出 → `cases/` 候选：场景指纹（DSL/SoC/shape/Bound）+ 机制 + 实测加速比 + 口径指纹绑定 + 证据等级 **E**（本工作流实测回灌；机制可信度等同 A 级，但 speedup 只绑定当时口径，不外推）。

---

# 10. 知识工程体系

## 10.1 五层知识（K1-K5）

| 层 | 内容 | 组织方式 | 本仓落点 | 现状 |
|----|------|---------|---------|------|
| K1 判定规则库 | 触发条件 + 证据要求 + 反证 + 置信度 + 预估收益 | 脚本化 + 绑定契约 + DAG | ops-verdict/rules | ✅ 23 条 + evals |
| K2 优化机制库 | 机制卡八要素 | 索引（按证据路由）+ 渐进式披露 | ops-mechanism/references/optimize | ✅ 7 张核心卡 |
| K3 算子族实践库 | 族范式 + 选型决策 + 模板代码 | A/B 双型 + 模板链路 | ops-family/references | ✅ 索引层（matmul/mc2 实质内容） |
| K4 案例证据库 | 场景指纹 + 机制 + 实测 + 证据等级 | 案例卡 schema + 路由规则 | ops-mechanism/cases | ✅ 3 张种子（含 1 反例） |
| K5 外部知识引用层 | 芯片参数 / API 文档 / 官方示例 / 理论标尺 | **引用清单 + 版本锁定，不落副本**（D9） | adapters/（引用清单）+ 运行时探测 | 🟡 框架期出口（标尺先行） |

**K1-K4 与 K5 的分界**（哪些知识自建、哪些引用外部）：

| 判据 | 自建（K1-K4） | 引用（K5） |
|------|--------------|-----------|
| 随芯片/API 演进？ | 否——「指标→瓶颈→机制」因果与 API 签名无关 | 是——参数/签名/示例每代都变 |
| 有无官方真源？ | 无（一线调优经验，本仓是唯一沉淀地） | 有（devkit 文档 / cann-bench metadata / 官方算子仓） |
| 更新驱动 | 实测回流（四回路） | 上游版本 bump + 引用清单更新 |
| 失效形态 | 阈值漂移（校准回路兜底） | 引用路径失效（CI 可达性校验兜底） |

> 判定阈值中源于官方文档的部分（`[msprof-doc]`）是混合形态：**阈值结论**自建在规则 YAML，
> **文档口径**引用外部——规则 YAML 的 threshold_source 字段即引用留痕。

## 10.2 渐进式披露（加载顺序与消费时机绑定）

```
1. 识别场景后    → 只读 tool-routing.md（采集选型）
2. 判定完成后    → 只读 findings 命中的机制卡（禁止判定前加载机制/案例猜瓶颈）
3. 方案确定后    → 只读对应族 guide + 模板表
4. 案例检索      → 最多 3 个同型（A-D/E 分级过滤）
5. 需要事实时    → K5 运行时探测/引用解析（芯片参数、API 签名、标尺数字），
                   禁止从记忆复述——与「判定只出自脚本」同构的「事实只出自真源」
```

## 10.3 阈值纪律与溯源

- 每条阈值带 `threshold_source`：`[msprof-doc]`（官方文档口径）/ `[tuning-practice]`（调优实践实测）/ `[mc2-practice]`（MC² 多卡实测）/ `🟡`（首期标定，M1.5 用真实调优数据校准）；
- 阈值变更流程：改 YAML → evals 全绿 → 来源标注 → 合入（**无来源合入在评审中降级为 watch 档**；缺反证的 BOUND 类规则不允许合入）；
- 跨芯片域：规则包 `soc` 过滤是硬边界；A2/A3/A5 阈值差异通过分包或（2027）规则内 soc 覆写承载，**禁止静默复用**。

## 10.4 自进化四回路与四纪律（2027 出口）

| 回路 | 内容 | 门禁 |
|------|------|------|
| 案例回流 | REPORT → case_candidate → cases/（脱敏 + E 级 + 查重 + 口径绑定） | 入库评审 |
| 校准闭环 | speedup_est vs 实测 → 逐机制卡回写 | 偏差看板跟踪 |
| 阈值进化 | 命中率/误报率统计 → 阈值调整建议 | **人审 + evals 全绿才生效** |
| 缺口感知 | 「暂未收录」计数 + 矩阵空白格 → 排期数据 | 周更看板 |

**四条防污染纪律**：① 进化只产生建议不直接生效；② 证据分级不可逆（E 级不覆盖官方 A 级规则）；③ 数据脱敏（剥离客户代码/敏感 shape）；④ 基线库版本化（跨环境比较禁律同样适用于基线库内部）。

## 10.5 知识扩展路径（每类知识怎么长）

| 知识类 | 新增一个条目的动作 | 门禁 | 规模目标（26 → 27） |
|--------|------------------|------|-------------------|
| K1 规则 | YAML 条目（阈值带溯源 + BOUND 类必带反证）+ fixture + evals 断言 | evals 全绿 + 人审 | 23 → 30+（融合算子档先行） |
| K2 机制卡 | 八要素写全 + 挂到对应规则的 mechanism_refs + 方案卡 schema 校验 | 评审（收益区间标 🟡） | 7 → 12+（tiling 细化/同步删减/SIMT/FA） |
| K3 族与模板 | 建族目录 → 判 A/B 型 → guide + 模板链路 + 族表更新；模板抄自官方的标来源与许可证 | 链路可点击到达 + 模板引用可达性校验 | 10 族索引 → 13 族（FA/conv/nn 补齐） |
| K4 案例 | 案例卡 schema + 证据分级 + 警示列 + 验证日期（反例同等收录） | 入库评审 | 3 → 40+（含 E 级回流） |
| K5 引用 | 引用清单加条目（路径 + 版本锁定）→ CI 可达性校验绿 | 引用可达 + 口径适配过回归 | 标尺 metadata 先行 → 全量 |

扩展的公共纪律：**新增知识必须先声明路由入口**（K1 挂 mechanism_refs / K3 挂族表 /
K5 挂 adapters 清单）——无入口的知识不可被路由到，等于不存在；「暂未收录」显式返回并
计数（缺口感知回路输入），禁止静默空结果。

## 10.6 AGENT/SKILL 自身更新的演进通道

知识扩展（§10.5）之外，**Agent/SKILL 框架本身**的更新也走机制不走自觉：

| 更新对象 | 触发 | 更新方式 | 回归保障 |
|---------|------|---------|---------|
| 事实性知识（芯片/API/示例/标尺） | 芯片演进 / AscendC API 演进 / devkit 升级 | **K5 引用 bump**（换版本锁定，不改本仓正文）；适配层核对列名/soc 映射 | U2 干跑全量 + 引用可达性校验 |
| 判定规则与阈值 | 新芯片特性（SIMT/VF/新 pipe）/ 误报反馈 | 规则 YAML 新增/标定 → evals 全绿 → 合入 | U1（阻断） |
| 机制卡与模板 | 新优化范式 / 官方示例更新 | 卡/模板新增或**追上游重拷**（不独立分叉） | 模板引用可达性 + U2 定向 |
| Agent 编排（AGENTS/task-prompts） | 流程缺陷 / 平台能力变化 | 模板修订 → 走评审 | U2 干跑联调用例（编排层唯一守护层） |
| SKILL 间契约（7 个 schema） | 消费方需要新字段 | 新增字段必须可选（向后兼容）；改义升 minor 版本 | evals + 契约互锁单元 |

> 判定/采集底座、评测（CANNBench）、知识真源（CANN 官方）三者都不归本仓维护——本仓
> 维护的是**适配层、契约与经验知识**。这个边界是「复用不重做」（P3）在演进维度的含义：
> 芯片与 API 演进的成本被外部真源吸收，本仓的演进成本集中在经验知识的生产上。

---

# 11. 关键场景端到端走查

## 11.1 单算子（Vector 类，如 GELU）

```
用户：工程 + cases.csv（20 case）+ 目标"耗时降到 100us 内"
  │
Phase 0: 跑工程正确性全集 → pass → scope.json
Phase 1: collect_board.sh --mode standard（7 组指标，逐 case）→ manifest
         snapshot.py: baseline/result.json（event 口径，warmup=5 repeat=20）
Phase 2: verdict.py（L2+L1 规则包 + PipeUtilization.csv）
         → 命中 L1_VEC_BOUND（vec_ratio mean 55%，light 档）
           + 反证 cast-heavy 命中（vec_fp32_ratio 61%）
Phase 3: mechanism_refs → api-usage.md（减 Cast）+ regbase-vf.md（A5）
         → 2 张方案卡（ub-fusion：模板✅可直接拷贝；vf-rewrite：模板⚠️部分实现）
Phase 4: 4a 两 implementer 并行实施；4b 主 agent 统一采集 → delta_compare
         → ub-fusion: 14.1→9.2us speedup 1.53x KEEP
         → vf-rewrite: 14.1→13.8us 噪声带内 → repeat 翻倍复测仍带内 → 无稳定收益处置
Phase 5: REPORT.md（六要素）+ E 级案例候选 + calibration_data
```

## 11.2 融合算子（MC²，如 alltoall_matmul）

```
Phase 0: 识别 fork 多 rank + CrossCoreFlag → 场景=融合算子；rankNum≤空闲卡数校验
Phase 1: collect_board.sh --mode mc2（task-based 强制）
Phase 2: ① isolation_test.py（源码副本注释编排）
           → T_comm=420us T_compute=880us R=2.10 → 强计算 bound
         ② 全瀑布 verdict.py（L3+L2+L1）
           → L3_MC2_STRONG_COMPUTE 命中
           → L3_MTE2_POLLUTED 命中（full=62 pure=41）→ L1_MTE2_BOUND 自动禁用
             （单核访存机制不出现在候选中——路由副作用的价值时刻）
           → L2_IMBALANCE_SEVERE 命中
Phase 3: mc2.md（Local Matmul 首选 + 配平）+ mc2-balancing.md（四剪枝 Top-N）
Phase 4: 配平候选逐个实测（跨 rank max 口径）→ 择优 KEEP；
         掩盖率核算 =（融合前−融合后）/min(T_comm,T_compute)
Phase 5: REPORT + E 级案例（MC² 场景指纹 → cases/mc2/）
```

## 11.3 调优问答（已有产物）

```
用户：给出已采集的 PROF 目录，问"为什么是 MTE2 Bound"
  │
仅 Phase 2: verdict.py 离线判定（manifest 存在 → 只解析不重采）
  → 输出判定书 + 四段式解读（结论与依据分开；反证逐条呈现；
     数据不足写"待确认"不写猜测；不适用写 N/A）
  → 禁止无证据输出 Top5；不修改源码
```

## 11.4 批量回归

```
用户：基线库已有 10 算子历史基线，要求回归体检
  │
Phase 1 简化: 基线直取（HARD_KEYS 校验环境一致，不一致 → 重建）
Phase 2: 重跑判定 + 同口径采集
Phase 4 改为: delta_compare 逐算子 → 回归门禁（speedup <0.95 红色告警）
             不做机制迭代（差异算子转单算子场景处理）
输出: 回归汇总表（逐算子 KEEP/劣化/噪声带）+ 告警清单
```

---

# 12. 目录结构与能力构成

## 12.1 本套件能力构成（自包含声明）

本套件自包含：判定规则（4 规则包）、机制卡、算子族实践文档与代码模板全部内置。外部依赖三类（工具链运行时引用、评测与标尺版本锁定引用、事实性知识 K5 动态引用——均不落内容副本）：

| 依赖类型 | 内容 | 用途 | 本套件落点 |
|---------|------|------|---------|
| CANN 工具链（运行时） | msprof / msprof op / msopprof / cannsim / npu-smi / acl | 采集与上板执行（复用不重做，本套件只做路由与契约） | ops-collect |
| 评测与标尺（框架期出口接入） | CANNBench（L1-L4 53 算子 + t_hw/baseline metadata + HAP） | 理论标尺与端到端用例取材 | Phase 1 标尺 / 评测（引用清单锁定版本） |
| CANN 知识源（K5 引用层，D9） | devkit API 文档 / 官方样例 / 芯片参数真源 / 官方算子仓 | 芯片参数·API 签名·示例代码·理论标尺的事实真源 | adapters/（引用清单 + 版本锁定 + 运行时探测） |
| NCU 机制对标（设计参考） | Guided Analysis（绑定契约 / DAG / Est.Speedup / 持久化） | 引擎机制设计对标（§3.2） | ops-verdict |

自建资产分布（全部版本化沉淀在本仓，随仓库演进）：

| 资产 | 规模（M1） | 位置（相对 skills/tool-ops-tune/） |
|------|-----------|------|
| 判定规则 | 24 条（L0-L3 四包） | ops-verdict/rules/ |
| 机制卡 | 7 张 + 索引 | ops-mechanism/references/optimize/ |
| 案例卡 | 3 张种子（含 1 反例） | ops-mechanism/cases/ |
| 算子族实践 | 10 族（matmul 六策略 / mc2 / reduce 5 模板 / broadcast 选型 / conversion 12 篇 / …） | ops-family/references/ |
| 代码模板 | 32 个（.h/.cpp/.template/.asc） | ops-family/references/*/templates/ 与 code/ |
| 故障分流 | 4 类 22 条 | agent/workflows/troubleshooting.md |

> 知识域资产按 PR 分期合入：判定规则包（L1/L2/L0 片上 → 多卡 L3）、机制卡与案例库、
> 算子族模板货架依次交付；合入前以各子域 SKILL.md 的资产计数台账为准。

## 12.2 目录结构

M1 骨架在本仓的实际落盘结构（自建资产分布见 §12.1）：

```
skills/tool-ops-tune/                    # 本套件（asc-tools skills 目录下单入口 skill）
├── SKILL.md                             # 套件入口：身份 + 五阶段总览 + 子域路由表
├── INSTALL.md                           # 三助手（opencode / Claude Code / Codex）安装指导
├── docs/                                # 工程文档
│   ├── 算子调优AGENT规划文档.md          #   场景与价值（配套文档）
│   ├── 算子调优AGENT设计文档.md          #   本文档（单一权威设计依据）
│   ├── 算子调优AGENT用例构建.md          #   分层用例体系（U0-U3）与 CI 触发设计（随测试框架 PR 交付）
│   └── figures/
│       ├── opstune-architecture.svg   #     四层架构图（§4.1 引用）
│       └── opstune-architecture.png   #     同图位图版
├── agent/                               # L3 Agent 层（编排，插件形态）
│   ├── AGENTS.md                        #   OpsTuningAgent（primary）：五阶段 + 门禁 + 纪律
│   ├── agents/
│   │   ├── perf-implementer.md          #   单机制实施 subagent（模板拷贝优先，不自采性能）
│   │   └── perf-reviewer.md             #   独立审查 subagent（消费 diff_audit 机器判定）
│   └── workflows/
│       ├── task-prompts.md              #   Step 0-5 调用模板（唯一执行手册）
│       └── troubleshooting.md           #   故障分流表（采集/编译/运行/对比 四类）
├── scripts/
│   └── diff_audit.py                    # 套件级脚本：实施范围机器校验（单变量铁律 diff 闸门）
└── <子域>/                              # L2 SKILL 层（可独立使用）
    ├── ops-collect/                     #   采集编排
    │   ├── SKILL.md
    │   ├── scripts/                     #     collect_board.sh / collect_cases.py / isolation_test.py / ...
    │   └── references/
    │       └── tool-routing.md          #     工具路由决策树 + 能力矩阵 + 踩坑
    ├── ops-verdict/                     #   ★判定引擎（核心增量）
    │   ├── SKILL.md
    │   ├── rules/                       #     规则包（opstune.rules/1.0，L0-L3 四包 24 条）
    │   ├── scripts/                     #     verdict / snapshot / delta_compare / batch_pipeline / trace_bubble / calibrate / render_html
    │   ├── references/                  #     rule-schema / thresholds / io-contracts / scope-schema / soc-spec
    │   ├── fixtures/                    #     已知答案样本（场景 CSV）
    │   └── evals/                       #     执行式回归 + 离线冒烟（CI 阻断语义）
    ├── ops-mechanism/                   #   机制与案例
    │   ├── SKILL.md                     #     机制卡八要素 + A-E 证据分级 + 降级策略
    │   ├── references/optimize/         #     机制卡 + 索引（rule_id → 卡映射）
    │   └── cases/                       #     案例卡（含反例）
    └── ops-family/                      #   算子族与模板货架
        ├── SKILL.md                     #     族分类表 + A/B 双型 + 模板纪律 T1-T5
        └── references/                  #     族目录 + common 跨族专题 + 模板 templates/ 与 code/
```

尚未落盘的规划目录（创建前以设计文档为准）：`adapters/`（K5 外部知识引用层——引用清单 + 版本锁定 + 运行时探测，见 §12.1 / D9，框架期落地）、U2 端到端用例（见《算子调优AGENT用例构建.md》，随测试框架 PR 交付）。

---

# 13. 扩展指南

## 13.1 新增一条判定规则（最常见扩展）

```
1. rules/<layer>-*.yaml 追加规则
   （threshold_source 必填；无实测来源标 🟡；BOUND 类规则必须带反证条目）
2. fixtures/ 造已知答案输入；evals/run_evals.py 加断言（正例 + ≥1 反例/边界）
3. python3 evals/run_evals.py 全绿
4. 若引入新机制方向 → ops-mechanism 补机制卡并挂 mechanism_refs
```

## 13.2 新增一个算子族

`ops-family/references/<family>/` → 判 A/B 型 → 更新族分类表 → 与机制卡互挂引用。fa / convolution / nn / random 族按此路径从 L4 实战案例补齐（框架期 FA 专项起步、自进化期补齐）。

**公共纪律（§10.5）**：新增任何知识先声明路由入口（族表/机制卡索引/mechanism_refs/adapters 清单）——无入口的知识不可被路由到；「暂未收录」显式返回并计数，禁止静默空结果。

## 13.3 新增一个 DSL（CATLASS / Triton / TileLang / PyPTO / SHMEM）

**判定引擎零改动**（规则层 DSL 无关——pipe 指标同构）；ops-mechanism 跨 DSL 原语对照表已有骨架；ops-family 增 DSL 分册；编译入口表扩充（每 DSL 一篇）。

## 13.4 接入编排平台

`agent/` 目录即插件形态：AGENTS.md（primary）+ agents/（subagent）+ workflows/（task-prompts）；frontmatter `skills:` 列表声明 SKILL 依赖，平台据此装配。SKILL 可独立使用（CLI/CI）是兜底形态——插件化是接入不是依赖。

## 13.5 扩展一条数据契约

契约 schema（`opstune.*/1.0`）变更走评审：新增字段必须可选（向后兼容）；删除/改义字段升 minor 版本；消费方（verdict/报告/CI）同步更新并过 evals。

---

# 14. 演进路线摘要

> 本节为里程碑级摘要；排期细节不在本文档维护范围。

## 14.1 两阶段目标（框架与突破 → 自进化）

| 阶段 | 目标定位 | 出口判定 |
|------|---------|---------|
| **第一阶段（M1.5 + 框架期）** | **框架与突破**——AGENT/SKILL 框架成型、测试用例框架成型、融合算子/FA 算子突破、性能数据 AI 亲和切换、HTML 报告 | AGENT/SKILL 框架（五阶段真机闭环）；测试用例框架（U0-U2 干跑 + evals 扩容全绿）；FA/MC² 全流程跑通；t_hw 标尺读数 + 采集产物结构化入库；HTML 报告（六区 + MC² 专属视图） |
| **第二阶段（自进化期）** | **自进化与完善**——AGENT/SKILL 自进化、性能测试用例完善 | 四回路上线（案例回流/校准/阈值进化/缺口感知）；预估-实测偏差 ≤30%；案例季增 ≥20；CI 门禁 ≥1 仓；外部贡献 ≥1；U3 矩阵 ≥80%→100% 已知格 |

**阶段内的里程碑切分**：

| 里程碑 | 时间 | 性质 | 核心出口 |
|--------|------|------|---------|
| **M1（本套件首期合入）** | 2026-10 | 骨架 | 判定引擎框架 + 执行式 evals 框架 + 四 SKILL + Agent 五阶段规范 |
| **M1.5 收口** | 2026-11~12（8 周） | 能用 | 真机 3 算子全流程；evals 扩容全绿；阈值标定项减半；2 份实产 REPORT |
| **框架期与突破** | 2026-11 ~ 2027-01 | 框架成型 + 融合算子突破 | FA/MC² 全流程（配平搜索脚本化 + 污染路由）；测试用例框架（U0/U2 干跑入 CI）；t_hw 标尺 + 采集产物 DBSqlite 化（AI 亲和切换收口）；HTML 报告 |
| **自进化期** | 2027-02 起 | 自我生长 | 自进化四回路；预估-实测偏差 ≤30%；案例季增 ≥20；CI 门禁 ≥1 仓；外部贡献 ≥1；U3 矩阵补齐；多 DSL / 知识 MCP（视资源弹性） |

## 14.2 框架期重点工作方向

按「框架优化（融合算子重点）/ 用例与 CICD / 端到端集成」三线推进：

| # | 方向 | 重点工作 |
|---|------|---------|
| 1 | **AGENT/SKILL 框架优化**（重点融合算子 FA/MC²） | ① MC² 配平搜索脚本化（四剪枝 + Top-N + TilingData 采集自动化）；② FA 专项（CrossCore flag 粒度 / GM buffer ping-pong / AIC:AIV 配比 → 混合绑核规则 + 机制卡 + E 级案例）；③ 融合算子判定档扩展（规则 24→30+：SIMT/VF 档、FA 混合绑核档）；④ K5 引用层搭建（标尺 metadata 接入 + 引用清单机制）；⑤ HTML 报告（六区 + MC² 通算流水图） |
| 2 | **性能用例与 CICD 流程** | ① U0 离线冒烟与单元回归（shim + 加载校验 + 契约互锁）；② evals 扩容（每规则 1 正 1 反）；③ U2 端到端用例（CANNBench L1-L4 取材 + 种子工程 + 干跑/真机双模式 + 六维断言器）；④ CICD 触发矩阵（变更路径→用例层级→阻断性：pre-push / 流水线 / 夜间真机 / 发版矩阵）；⑤ TTFI 打点与判定误报率统计 |
| 3 | **端到端集成验证** | ① agent/ 插件形态接入编排平台（frontmatter skills 装配 + 权限模型联调）；② 五阶段在平台内真机跑通（2 算子全流程含 1 轮 REVERT）；③ Toolkit 可视化联动（profiling 可视化 → 判定书呈现 → 报告跳转链路）；④ 双入口同源验证（CLI 与平台消费同一 SKILL 集与契约，P4）；⑤ 集成问题三分类回流（框架 bug / 用例缺口 / 平台问题） |

**三线交界的契约**：方向 2 的用例是方向 1 框架改动的回归网（框架改动过 U0-U2 才合入），
方向 3 的集成发现按「框架 bug / 用例缺口 / 平台问题」三分类回流到方向 1/2——三条线通过
已冻结的 rule-schema.md 与用例卡 schema 衔接，不需要额外协调机制。

**优先级**（冲突时取舍）：融合算子 FA/MC² 全流程 > 用例框架（U0/U2 干跑先行，
真机层随框架联调节奏）> 平台集成（依赖平台排期，风险由「SKILL 可独立使用」
架构兜底）> HTML 报告与多 DSL（弹性范围）。

## 14.3 自进化期滚动方向

框架线转自进化（四回路 + 阈值进化门禁 + 外部贡献通道）；用例线补 U3 场景矩阵与算子仓
CI 门禁（自进化期出口项）；集成线视编排平台推广情况扩展多仓接入。「多 DSL 全量补齐、
知识 MCP、多 DSL 案例库」等规模化项按缺口感知数据排产，非固定投入。

---

# 15. 风险与对策

| # | 风险 | 等级 | 对策 | 关联 |
|---|------|------|------|------|
| R1 | MC² 多卡 replay 挂死（NCU 官方点名同类问题：强制并发 kernel 多 pass 采集可能无法推进） | 高 | 框架期初即实测；不可行则固化 msprof task-based（现状已可用，非阻断） | 框架线 |
| R2 | 规则误报损害信任 | 高 | 反证机制内置每条规则；置信度分级；evals 回归；初期 watch 档灰度输出 | M1.5 / 阈值进化 |
| R3 | FA 知识沉淀受案例获取制约 | 中 | 专项投入；案例先 E 级入库验证后升格；attention 变体取材 | 框架线 |
| R4 | 阈值跨芯片失效（A2/A3/A5） | 中 | 规则包 soc 域硬过滤；每芯片维护 fixture；REQUIRED/OPTIONAL 分级降级；芯片参数走 K5 引用不落副本（D9） | 全程 |
| R5 | 知识生产速度跟不上 | 中 | 缺口感知数据排产；案例回流降边际成本；外部贡献通道；聚焦融合算子不铺全量 DSL | 全程 |
| R6 | 编排平台联调排期不可控 | 中 | SKILL 可独立使用是架构兜底；集成线专责推动，不阻塞框架线与用例线 | 集成线 |
| R7 | LLM 幻觉污染判定 | 中 | 架构性根除（D1）：判定只出自脚本，LLM 只解释——判定书带证据引用可审计 | 已落地 |
| R8 | 评测与真实场景偏差 | 中 | U2 从真实调优案例取材；TTFI 真人计时打点（非脚本时长） | 用例线 |
| R9 | 范围蔓延 | 中 | 优先级显式化（§14.2）；多 DSL/MCP 降为弹性项按缺口数据排产；里程碑门禁「只降范围不放水」 | 排期 |
| R10 | 外部知识源（devkit/metadata）版本漂移导致引用失效 | 中 | K5 引用清单 + 版本锁定 + CI 可达性校验；上游 bump 须过 U2 干跑回归（D9） | 框架线 |

---

# 16. 附录

## 附录 A：术语表

| 术语 | 释义 |
|------|------|
| Bound | 瓶颈类型判定：VEC/CUBE/MTE2/MTE3/FIXP/SCALAR（单核 pipe 级）、LATENCY/SYNC（全 pipe 低占用）、计算/通信五档（MC²） |
| 绑定契约 | 规则声明 REQUIRED/OPTIONAL 指标；REQUIRED 缺失 → 规则 SKIPPED（未采集不判定） |
| 三值逻辑 | 条件求值 ∈ {True, False, None}；None=证据不足 → INSUFFICIENT_DATA，不冒充 False |
| 路由副作用 | 规则命中后禁用其它规则/改道机制路由（如 MTE2 污染禁用单核 MTE2 判定），跨规则包传播 |
| 分层瀑布 | 判定顺序 L3 卡间 → L2 核间 → L1 单核 → L0 指令；上层结论约束下层 |
| 口径指纹 | snapshot 的 HARD_KEYS 12 字段 + MECHANISM_KEYS 2 字段；不一致禁止比较（delta_compare exit 2） |
| 噪声带 | ±5% 内不判收益也不判劣化 → repeat 翻倍复测；run 间噪声 >5% 时小于噪声的收益不得归因 |
| 隔离测试 | tileCnt=1 下分别注释通信/计算 process，实测 T_comm/T_compute 的实验方法（源码副本中进行） |
| R = T_compute/T_comm | MC² 通算比；五档：≈1 平衡 / (1.1,2] 计算 / >2 强计算 / <1 通信 / <0.5 强通信；实测跨 0.3~9.8 |
| 配平比 ρ | max(R, 1/R)，长短块 M 大小之比（配平搜索输入） |
| 掩盖率 | (融合前−融合后)/min(T_comm,T_compute)，衡量通算重叠，上限 1.0 |
| 跨 rank max | 多 rank 性能取各 rank 稳态均值的最大值（木桶效应：整体性能由最慢 rank 决定） |
| MTE2 污染 | CrossCoreWaitFlag 挂 MTE2 流水线导致 aic_mte2_ratio 含通信等待空转；判定为真则单核访存优化无效 |
| 串行指纹 | 多 pipe 互补求和≈100%（如 cube .44+mte2 .42+mte1 .18）→ 单缓冲串行无重叠 |
| 证据分级 | 案例库 A（官方 PR 含数字）/B（官方 PR）/C（官方指南）/D（社区二手）/E（本工作流实测回灌） |
| 降级策略 | 无同型案例时：证据链+机制卡驱动、实施细节自行推导并提高验证强度、报告标注、沉淀 E 级候选 |
| 单变量铁律 | 每轮只改一个可归因机制；先精度后性能；噪声带内不归因；负优化可回滚可交代 |
| 负优化四机制 | 可预防（须有当前算子证据）/ 可检测（同口径+噪声带）/ 可回滚（diff+复跑验证）/ 可交代（记录进报告） |
| 语义风险分级 | preserved（分核/双缓冲/搬运合并/同步删减）常规门禁；altered（算法替换/dtype/归约顺序）附边界用例 |
| HARD_KEYS / MECHANISM_KEYS | 口径字段（禁比）/ 机制变量（WARN）——tiling/block_dim 优化本身会改变后者 |
| TTFI | Time To First Insight：交付样本到首个含判定+动作结论的耗时（北极星，真人打点） |
| t_hw_us | CANNBench 硬件理论下界（metadata 外置），理论标尺锚点 |
| DBI | 动态二进制插桩（msopprof 逐 PC/指令级数据来源） |
| CrossCore Flag | AIC↔AIV 跨核同步原语（MC² Pattern A/B 同步方向载体） |
| K5 引用层 | 外部事实性知识（芯片参数/API/示例/标尺）的引用清单与版本锁定层（D9）；运行时探测优先，不落内容副本 |
| 五级路由 | R1 场景 → R2 采集模式 → R3 判定层 → R4 机制 → R5 族（D10）；决策载体均为结构化产物，LLM 只查表不发明 |

## 附录 B：阈值溯源摘要

> 全量溯源见 `ops-verdict/references/thresholds.md`（相对本套件根目录）。标注体系：`[msprof-doc]` = 官方文档口径；`[tuning-practice]` = 调优实践实测；`[mc2-practice]` = MC² 多卡实测；`🟡` = 首期标定值（M1.5 校准后回写）。

| 类别 | 关键阈值 | 来源 |
|------|---------|------|
| 主 Bound 判定 | >80% 直接判 / 最大且 >70% | [msprof-doc] |
| VEC 三档 | 50-65 轻度 / 65-80 中度 / >80 接近极限 | [tuning-practice] |
| MTE2 真饱和反证 | active_bw ≥ 峰值 80%（原文「差异<20%」折算） | [tuning-practice] |
| FIXP 对齐预警 | >15% = 地址未 512B 对齐 | [tuning-practice] |
| LATENCY/SYNC | 全 pipe mean <50% | [tuning-practice] |
| 串行指纹 | pipe 求和 ∈ [85,115]%（容差扩展 🟡） | [tuning-practice] 案例 |
| 核间长尾 | max > mean×1.25 | [tuning-practice] |
| 离散度三档 | (max−min)/max <10 / 10-30 / >30 | [tuning-practice] |
| 切核不足 | 活跃核 < 物理核 33%（A5 约 48 AIV/28 AIC） | [tuning-practice] + 分档 🟡 |
| 头开销 | (Task−max(aic,aiv))/Task >30%；A2 满核 ~20-21us | [tuning-practice] |
| R 值五档 | [0.9,1.1] / (1.1,2] / >2 / [0.5,1) / <0.5 | [mc2-practice] |
| MTE2 污染 | full−pure >10pp 或 pure/full <80% | [mc2-practice] |
| 噪声带 | ±5% | [tuning-practice] 负优化四机制 |
| 冷稳态差 | 实测可达 25%（固定 warmup 剔除） | [tuning-practice] 计时口径实测 |
| MC² 稳态 | 循环 ≥10 前五 warm-up；跨 rank max | [mc2-practice] |

## 附录 C：参考资料

| # | 材料 | 用途 |
|---|------|------|
| 1 | 昇腾算子仓（ops-nn / ops-transformer / ops-math / ops-cv）与 cann-samples | 算子实现参考；优化实践来源 |
| 2 | Ascend C 官方文档与样例（含性能实践系列） | API 约束与调优实践来源 |
| 3 | msOpProf 用户指南（github.com/Ascend/msopprof） | 采集工具口径与能力边界 |
| 4 | msOpProf（github.com/Ascend/msopprof） | 采集底座；device/simulator 双模 |
| 5 | CANNBench（gitcode.com/cann/cann-bench：tasks L1-L4 53 算子 + metadata t_hw/baseline） | 评测体系与理论标尺 |
| 6 | Nsight Compute 官方文档 + 2026.2.1.5 安装包实测（24 规则源码级拆解） | 规则引擎对标（绑定契约/DAG/Est.Speedup） |
| 7 | 一线算子调优实战案例沉淀（MC² 多卡 / Vector / MatMul / FA 类） | 阈值标定与机制卡内容来源 |

## 附录 D：设计决策速查表

| 决策 | 一句话 | 落点 |
|------|--------|------|
| D1 | 判定进脚本、解释进 LLM | ops-verdict + Agent 边界 |
| D2 | 规则-指标绑定契约 | metrics_required/optional → SKIPPED/降级 |
| D3 | 分层瀑布 + 路由副作用 | L3→L2→L1→L0；污染禁用单核判定；None 不冒充 False |
| D4 | 口径指纹互锁 | snapshot HARD/MECHANISM keys + delta_compare exit 2 |
| D5 | 五阶段主循环 + 单变量铁律 | AGENTS.md Phase 0-5 |
| D6 | 知识按消费时机分层 | 机制卡判定后才加载；边界声明头部化 |
| D7 | 职责隔离编排 | implementer 单机制；主 agent 垄断采集与对比 |
| D8 | 执行式评测 | evals/run_evals.py CI 阻断语义 |
| D9 | 外部知识动态引用 | K5 引用层：事实性知识引用 CANN 真源不落副本；运行时探测优先 |
| D10 | 五级路由瀑布 | R1 场景→R2 采集→R3 判定层→R4 机制→R5 族；路由进表格不进记忆 |

---

*OpsTune 算子调优 AGENT 架构设计文档 · 2026-10 · V1.1 ｜ 判定进脚本 · 解释进 LLM · 经验可回流*
