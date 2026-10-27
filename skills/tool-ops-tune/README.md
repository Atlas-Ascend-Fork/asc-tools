# OpsTune — 昇腾算子性能调优 AGENT/SKILL 套件

![License](https://img.shields.io/badge/License-CANN--2.0-blue?style=flat-square)
![Python](https://img.shields.io/badge/Python-3.8%2B-3776ab?style=flat-square&logo=python&logoColor=white)
![Platform](https://img.shields.io/badge/Platform-Ascend%20NPU-orange?style=flat-square)
![Evals](https://img.shields.io/badge/evals-38%2F38%20passing-brightgreen?style=flat-square)
![Smoke](https://img.shields.io/badge/smoke-13%20checks%20all%20green-brightgreen?style=flat-square)
![Agent Skills](https://img.shields.io/badge/Agent%20Skills-5%20%E5%AD%90%E5%9F%9F-blueviolet?style=flat-square)

**判定进脚本 · 解释进 LLM · 经验可回流**

📖 [架构设计](docs/ops-tune-design.md) ｜ [知识库规范](docs/知识库规范.md) ｜ [评测体系](docs/用例构建.md) ｜ [安装指导](INSTALL.md)

给「已可运行、精度通过」的昇腾算子做性能调优的完整 Agent + SKILL 工具链：分层瓶颈判定引擎（规则-指标绑定契约）、机制卡与案例证据库、算子族模板代码货架、MC² 隔离测试与配平搜索、调优主循环（采集→问题判定→机制路由候选方案→单变量执行优化→对比确认）——让 Bound 判定可审计、阈值可回归、结论可复现。

---

## 📢 为什么需要 OpsTune

算子性能调优长期是专家驱动的手工活：一次 MTE2 Bound / VF 级别的专家诊断常以人月计，判定质量随人波动，经验随人员流动流失。OpsTune 把这条链路工程化：

| 业界现状痛点 | OpsTune 的答案 |
|-------------|---------------|
| Bound 判定靠 LLM/专家读文档，阈值变更不可回归 | **ops-verdict 判定引擎**：24 条专家规则 YAML 化 + 脚本求值 + 执行式回归（evals 38 用例） |
| 指标没采也可能被「判」出瓶颈 | **规则-指标绑定契约**：REQUIRED 缺失 → SKIPPED，机制上杜绝无证据判定 |
| MC² 场景 MTE2 被通信等待污染，单核优化做了白做 | **路由副作用**：污染判定命中自动禁用单核访存规则并改道通信掩盖策略 |
| 跨口径比较、噪声带内宣称收益 | **口径指纹互锁**：12 字段 HARD 校验 + 精度门禁 + ±5% 噪声带三段结论 |
| 融合算子隔离测试全靠手工 | **isolation_test.py**：T_comm/T_compute → R 值实验编排（跨 rank max 木桶口径内置） |
| 经验不回流 | **案例卡 schema**（A-E 证据分级）+ E 级回灌 + 预估-实测校准数据随每轮产出 |

**面向用户**：昇腾 NPU 算子开发者（Ascend C 为主）、性能工程师、算子仓 CI 维护者。

---

## 🏆 核心竞争力

### ① 性能数据 AI 亲和性设计

采集产物即 Agent 可直接消费的结构化数据——**AI 读得更快、更省 Token，判定更快更准**：

- **950 平台采集对接 [npu-compute](../tool-npu-compute/)**（本仓 npu_tools/npu_compute）：相比传统 msprof 导出，npu-compute 产出结构化 `.npu-rep` 报告（7 Section CSV + `Summary.jsonl` + **`summary.db`** SQLite），Agent 侧零解析成本直接喂判定引擎；
- **DB 格式消费链已打通**（Roadmap #4 交付）：`summary.db` 聚合查询下推（AVG+NULL-skip）使标量提取与分析查询**实测提速一个数量级**（`db_bench.py` 四负载等价性门禁 + 提速比，复跑命令见 [ops-collect/SKILL.md](ops-collect/SKILL.md)）；`PcSampling` 表（DB 独有）产出 `top_stall_pct`——L0_STALL_DOMINANT 规则的首个生产者；
- **采集维度更全**：涵盖 **Block Profiling**（逐 Block 核间负载形态——L2 核间判定层的直接证据源）、**通信性能数据**（多卡/集合通信时延与带宽——MC² 卡间判定层输入）、**SIMT WARP 流水灯**（Warp 调度与发散形态——条件分支/离散索引类算子的指令级证据）——为多卡、多核、单核全场景性能调优提供更完整的证据链；
- **指标词汇表与三代列名兼容**（aic_/aiv_/ai_）+ ratio_to_pct 双口径归一 + mean/max 双统计内建于引擎——性能数据从「人读的 CSV」升级为「机器判定的契约数据」。

### ② TOP-DOWN 分层判定方法论，对标业界领导者

对标业界领先 GPU Profiling 工具 Guided Analysis 的「规则引擎 + 绑定契约 + 预估加速比排序」形态，并向前一步——**判定进脚本，速度更快、结论更准、质量不随模型波动**：

- **L3 卡间 → L2 核间 → L1 单核 → L0 指令** 四层瀑布，上层结论约束下层（TOP-DOWN 归因路径，杜绝单核视角误判融合算子）；
- **内置 24 条专家经验规则**（MC² R 值五档 / 核间长尾离散度三档 / MTE2 真假 bound 二分 / 串行无重叠指纹 / 头开销 / host 下发阻塞…），每条带**阈值溯源 + 反证条目 + 置信度 + 预估收益区间 + 机制路由**；
- **可扩展零代码**：新规则 = 一个 YAML 条目 + fixture + 断言用例（evals 全绿才可合入）——专家经验的积累不碰引擎；
- **覆盖多卡、多核、单核全场景**：单算子 / 融合算子（FA·MC²）/ 批量回归 / 调优问答四类入口共用同一规则集与数据契约。

### ③ 专业知识体系：分类化 · 结构化 · 可演进

算子调优知识不是散落的文档，而是**分层分类、带路由入口、可检索可扩展**的知识资产：

- **知识五层分类（K1-K5）**：判定规则（24 条 YAML）/ 优化机制卡（8 张，固定八要素：原理·前提·收益·架构限制·精度风险·验证命令·回滚条件·模板资产）/ 算子族实践（11 族 A/B 双型文档 + 32 个可拷贝代码模板 + 计时 harness ×2）/ 案例证据（A-E 分级，反例同等收录）/ 外部事实引用（CANN 真源动态引用不落副本）；
- **多层次知识体系 + 生态对接**：经验性知识沉淀于本套件，事实性知识（芯片参数 / API 签名 / 官方示例 / 理论标尺）对接 [cann-knowledge](https://gitcode.com/cann/cannbot-knowledge) 知识库真源——单一事实源，不维护会腐烂的副本；
- **知识自演进四回路**：案例回流（E 级实测入库）/ 校准闭环（预估 vs 实测偏差回写收益区间）/ 阈值进化（命中率统计 → 人审 + 回归全绿才生效）/ 缺口感知（「暂未收录」显式计数 → 排产信号）；
- **知识轨迹能力**：每张判定书、每轮对比、每条回灌案例都带口径指纹与证据链可追溯——知识从哪来、被谁用过、验证过几轮，全程留痕。

### ④ 算子性能 Benchmark 构建，评测与自进化双驱动

调优能力本身也需要被评测——Benchmark 是 AGENT/SKILL 自进化的**量化反馈回路**：

- **执行式回归体系**：evals 38 用例（判定回归 E1-E26 含 R 值临界值双侧守护 + 门禁脚本回归 S1-S12）+ 离线冒烟 smoke 13 项检查（msprof shim 无 NPU 可跑）+ 触发评测 7 用例——规则/阈值任何变更，判定质量劣化立即被抓住；
- **真机标尺**：soc-spec 峰值表（带宽/算力/容量，来源可溯）提供「还差多远」的客观读数；CANNBench L1-L4 任务的 t_hw 理论下界可作外部标尺接入；
- **自进化闭环**：Benchmark 评测数据（误报率 / 命中率 / 预估偏差）→ 阈值进化建议 → 人审 + 回归全绿 → 合入——AGENT/SKILL 的能力提升走机制不走自觉。

---

## 🏗️ 项目架构

四层架构：**Agent 编排（只调度不判定）→ SKILL 能力（确定性沉淀）→ 规则引擎（判定进脚本）→ CANN 工具链（复用不重做）**。

```
skills/tool-ops-tune/
├── SKILL.md                    # 套件入口：主循环总览 + 子域路由表
├── INSTALL.md                  # 三助手（opencode/Claude Code/Codex）安装指导
├── agent/                      # L3 Agent 编排层
│   ├── AGENTS.md               #   OpsTuningAgent：调优主循环 + 三道精度门禁
│   ├── agents/                 #   perf-implementer / perf-reviewer subagent
│   └── workflows/              #   task-prompts（Step 执行手册）+ troubleshooting
├── ops-collect/                # L2 采集编排（模式路由 + manifest 契约 + MC² 隔离测试）
├── ops-verdict/                #   ★ 判定引擎（规则包 24 条 + snapshot/delta_compare
│   │                           #     口径互锁 + soc-spec 标尺 + evals/smoke 回归）
├── ops-mechanism/              #   机制卡 8 张 + 案例库（A-E 分级）+ mc2_search 配平搜索
├── ops-family/                 #   算子族 11 族 + 32 个可拷贝模板 + tiling 候选生成器
└── ops-report/                 #   结果可视化（render_html 单文件静态报告）
```

**调优主循环**：`（前置）意图与边界：精度门禁预检 + 场景分类 → 【采集 → 问题判定（verdict.json 分层瀑布）→ 机制路由（候选方案卡 ≤3）→ 单变量执行优化（diff_audit 机器闸门）→ 对比确认（KEEP/REVERT/复测三段结论）】循环至收敛 → （收尾）归档回灌（REPORT 六要素 + E 级案例）`

## 🚀 快速开始

### 前置条件

- 任选一种 AI 编程工具：[Claude Code](https://code.claude.com/docs/en/setup) / OpenCode / Codex
- Python ≥ 3.8 + PyYAML（判定引擎唯一硬依赖）
- 上板采集需 CANN 工具链（msprof / npu-compute / npu-smi）；**纯判定 / 回归 / 问答无需 NPU**

### 30 秒体验：不装 Agent，直接判定

```bash
# 拿一份 msprof 导出的 PipeUtilization.csv，一句话出判定书：
python3 ops-verdict/scripts/verdict.py \
    --rules ops-verdict/rules/L2-inter-core.yaml,ops-verdict/rules/L1-single-core.yaml \
    --csv PipeUtilization.csv --soc a5 \
    --scalar physical_cores=48,block_dim=24,task_duration_us=195 \
    -o verdict.json
```

```
verdict: NEEDS_OPTIMIZATION  |  MTE2 搬入瓶颈（moderate）；另有 2 项命中
  HIT  [moderate/high  ] L1_MTE2_BOUND: MTE2 搬入瓶颈  (est [1.2, 2.0])  evidence: mte2_ratio=78.0
  HIT  [critical/high  ] L2_IMBALANCE_SEVERE: 核间负载不均衡（离散度严重档）  (est [1.1, 1.5])
  skipped=0  insufficient_data=0
```

每条命中都带**证据数值 + 阈值溯源 + 反证评估 + 置信度 + 预估收益 + 机制路由**——判定可审计，不是黑盒。

### 用自然语言调优（装好 Agent 后）

```text
"帮我调优这个 gelu 算子，cases.csv 里有 12 个 case，目标是 100us 以内"
"这个 MC² 算子为什么慢？给出瓶颈判定和证据"
"把优化前后的 profiling 对比一下，生成报告"
```

Agent 按主循环执行：采集与基线固化 → 问题判定 → 机制路由候选方案 → 单变量执行优化 → 对比确认（KEEP/REVERT），循环至收敛后 REPORT 归档，全程产物落 `tuning_artifacts/{operator}/`。

安装方式（opencode / Claude Code / Codex 三助手）与加载验证详见 **[INSTALL.md](INSTALL.md)**。

## 📊 资产总览

| 资产 | 规模 | 位置 |
|------|------|------|
| 判定规则（L0-L3 四包） | 24 条（阈值全溯源，BOUND 类必带反证） | ops-verdict/rules/ |
| 优化机制卡 | 8 张（固定八要素，含 MC² 配平搜索） | ops-mechanism/references/optimize/ |
| 案例证据卡 | 3 张（A-E 分级，含反例） | ops-mechanism/cases/ |
| 算子族实践 + 模板 | 11 族 + 32 个可拷贝代码模板（族表断言零漂移）+ 计时 harness ×2 | ops-family/references/ |
| 执行式回归 | evals 38 用例 + smoke 13 项检查（无 NPU 可跑） | ops-verdict/evals/ |
| 数据契约 | 7 份 schema（collection/verdict/snapshot/scope/candidates/isolation/case） | 各子域 references/ |

## 🗺️ Roadmap

| # | 方向 | 说明 |
|---|------|------|
| 1 | **性能报告解读 SKILL（AscendC Toolkit 插件）** | 对接 AscendC Toolkit 插件形态：读取性能报告，输出「性能问题 + 证据 + 修复建议」三段式解读，可引用本套件（ops-tune）的判定引擎与机制卡作为依据——把专业判定能力带到开发者日常工作流 |
| 2 | 补充最佳实践案例 | 案例证据库扩容（A/B 级官方 PR 案例 + E 级实测回流），覆盖更多算子族与融合形态 |
| 3 | 优化 HTML 报告 | render_html 增强：交互式钻取、Bound 演化时间线、多轮迭代对比视图 |
| 4 | ~~采集工具切换 DB 格式数据~~ **✅ 已交付** | npu-compute 采集产物 sqlite DB 格式消费（`--mode npu-compute` 采集编排 + facts_bridge DB 通道 + verdict `--db` + csv_to_db 升级器 + db_bench 性能验证）；PcSampling → top_stall_pct 打通 L0_STALL_DOMINANT |
| 5 | 知识标准化 OKF 格式对接 CANN-KNOWLEDGE | 知识资产按 OKF（Open Knowledge Format）标准化，与 [cann-knowledge](https://gitcode.com/cann/cannbot-knowledge) 双向对接——本套件知识可被生态检索与复用 |
| 6 | AGENT/SKILL 增加 trace 能力 | 判定与调优全程 trace 留痕（决策链可视化）：每步为什么这么做、证据是什么、替代方案为何未选 |
| 7 | 新芯片 / API 适配 | 适配下一代芯片与新 AscendC API：soc-spec 峰值表 / 规则包 soc 域 / 列名兼容层随芯片演进扩展 |

## 🤝 贡献

- **新增判定规则**：YAML 条目（threshold_source 必填 + BOUND 类必带反证）+ fixture + evals 断言，回归全绿后提 PR——详见 `docs/知识库规范.md`；
- **新增机制卡 / 案例卡 / 算子族模板**：按 `docs/知识库规范.md` 的八要素与 A-E 证据分级书写；
- 提交流程遵循 [CONTRIBUTING.md](../../CONTRIBUTING.md)。

## 📄 许可证

[CANN Open Software License 2.0](../../LICENSE)
