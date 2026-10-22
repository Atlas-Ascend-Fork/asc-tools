---
name: tool-ops-tune
description: 昇腾算子性能调优 AGENT/SKILL 套件（OpsTune）。对「已可运行、精度通过」的算子工程做性能调优：分层瓶颈判定引擎（规则-指标绑定契约，判定只出自脚本）、性能采集编排（msprof 模式路由与 manifest 契约）、机制卡与案例证据库、算子族模板货架、五阶段调优工作流（基线→分层判定→机制路由→单变量迭代→对比归档）。触发：算子调优、瓶颈判定、Bound 判定、性能采集、profiling 编排、调优前后对比、加速比结论时使用。
---

# 昇腾算子调优套件（OpsTune）

> **判定进脚本 · 解释进 LLM · 经验可回流**
> 给「已可运行、精度通过」的昇腾算子做性能调优的完整 Agent + SKILL 工具链：
> Bound 判定可审计、阈值可回归、结论可复现。

## 套件总览（五阶段调优闭环）

```
工程 + cases（精度已通过）
    │
Phase 0 意图与边界 ─ 精度门禁预检（全集跑通）+ 场景分类（单算子/融合/回归/问答）→ scope.json
    │
Phase 1 采集与基线 ─ ops-collect 模式路由（MC² task-based 强制/单卡 standard/无卡 sim）
    │                 + 不可变基线 snapshot（口径指纹 12 HARD 字段）
    │
Phase 2 分层判定 ── ops-verdict 规则引擎：L3 卡间→L2 核间→L1 单核→L0 指令瀑布
    │                 + 路由副作用（MTE2 污染自动禁用单核访存判定）→ verdict.json
    │
Phase 3 机制路由 ── 按 findings[].mechanism_refs 加载机制卡（判定后才加载，证据先行）
    │                 → 候选方案卡 ≤3（机制+证据+diff 草案+预估收益+回滚条件）
    │
Phase 4 单变量迭代 ─ perf-implementer 单机制实施 → diff_audit 范围机器校验
    │                 → perf-reviewer 独立审查 → 同口径统一采集 → delta_compare
    │                 三段结论（KEEP / REVERT / 噪声带复测）
    │
Phase 5 归档回灌 ── REPORT 六要素 + E 级案例候选 + 预估-实测校准数据
```

编排规范：`agent/AGENTS.md`（OpsTuningAgent 五阶段主循环 + 三道精度门禁 + 口径铁律）；
Step 执行手册：`agent/workflows/task-prompts.md`；故障分流：`agent/workflows/troubleshooting.md`。

## 子域路由表（按调优阶段加载）

| 子域 | 回答的问题 | 关键资产 | 交付状态 |
|------|-----------|---------|---------|
| `ops-collect/` | 数据怎么采对采全 | 采集模式路由决策树 + collect_board/collect_cases/facts_bridge/jsonl_to_cases/collect_sim + manifest 契约（opstune.collection/1.0） | ✅ 本期交付（MC² 隔离测试随多卡域 PR 交付） |
| `ops-verdict/` ★ | 瓶颈是什么、为什么不是别的；改好了吗 | 判定引擎 6 脚本 + L0-L3 规则包 24 条 + 口径指纹互锁（snapshot/delta_compare） + soc-spec 峰值标尺 + 执行式回归（evals 38 用例 + 离线冒烟 smoke） | ✅ 已交付 |
| `ops-mechanism/` | 怎么改、能涨多少、风险在哪 | 机制卡八要素 + 案例证据库（A-E 证据分级） | 🟡 随优化卡片知识 PR 交付 |
| `ops-family/` | 代码怎么写（模板货架） | 算子族 A/B 双型文档 + 可拷贝代码模板 + tiling 候选生成器 | 🟡 随算子族模板 PR 交付 |
| `ops-report/` | 结果怎么呈现 | 调优结果可视化报告（单文件静态 HTML） | 🟡 随报告 PR 交付 |

> 子域间唯一耦合点是显式契约引用：verdict 的 `mechanism_refs` → 机制卡；方案卡的
> `template` → 模板路径；manifest `blocks` → CSV 输入定位；snapshot HARD_KEYS →
> delta_compare 校验域。契约字段不变则两侧互不影响。

## 30 秒体验：不装 Agent，直接判定

```bash
# 拿一份 msprof 导出的 PipeUtilization.csv，一句话出判定书：
python3 ops-verdict/scripts/verdict.py \
    --rules ops-verdict/rules/L2-inter-core.yaml,ops-verdict/rules/L1-single-core.yaml \
    --csv PipeUtilization.csv --soc a5 \
    --scalar physical_cores=48,block_dim=24,task_duration_us=195 \
    -o verdict.json
```

每条命中都带**证据数值 + 阈值溯源 + 反证评估 + 置信度 + 预估收益 + 机制路由**——判定可审计，不是黑盒。

## 采集入口（上板 / 仿真）

```bash
# 上板标准采集（7 组 aic-metrics 顺序采集 + manifest 契约落盘）：
bash ops-collect/scripts/collect_board.sh --mode standard --exe ./build/demo --args "8448 4096 4096" -o PROF_OUT

# 逐 case 批量采集（cases.csv → perf_data.csv，batch_pipeline 直连）：
python3 ops-collect/scripts/collect_cases.py --cases cases.csv --exe ./build/demo -o collect_out

# 采集产物 → 判定标量桥接（L0 规则与 L1 反证的标量生产者）：
python3 ops-collect/scripts/facts_bridge.py --prof PROF_OUT --soc a5
```

## 安装

三种 AI 助手（opencode / Claude Code / Codex）的安装与加载验证见 [INSTALL.md](INSTALL.md)。

## 职责边界

| 做 | 不做 |
|---|---|
| 对「可运行且精度通过」的算子做性能调优闭环 | 不做算子开发（从零开发走算子开发域工作流） |
| 判定进脚本（verdict.py 规则求值，LLM 只解释） | 不做精度调试（精度失败 → 停止调优转调试域） |
| 同口径前后对比（口径指纹互锁 + 噪声带三段结论） | 不重做采集器（msprof/msopprof/cannsim 全部复用） |
| 经验回流（E 级案例 + 校准数据 + 缺口计数） | 不在噪声带（±5%）内宣称收益 |
