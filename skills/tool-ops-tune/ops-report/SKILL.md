---
name: ops-report
description: 调优结果可视化报告。把 tuning_artifacts（verdict 判定书 + snapshot 口径指纹 + comparison 矩阵 + trace/重叠度证据）转换为单文件静态 HTML 报告：汇总卡片（geomean/KEEP/REVERT）、逐 case speedup 热力表、判定书证据段、逐 case 对比明细折叠。离线可打开、零外部依赖。触发：调优结束生成报告、结果可视化、汇报材料准备、矩阵人读不动时使用。
---

# 调优结果可视化报告（ops-report）

> **来源（蒸馏）**：报告形态与模块合同蒸馏自 同类工具实践
> （独立 HTML 报告 + 数据解析/渲染分离 + 缺失如实呈现三原则）；数据真源与口径
> 校验是本仓自有（verdict/snapshot/delta_compare 契约），渲染入口为
> `ops-verdict/scripts/render_html.py`（P2-3 交付）。

## 职责边界

| 做 | 不做 |
|---|------|
| tuning_artifacts → 单文件静态 HTML（离线可开、零外部依赖） | 不重新采集/重新判定（数据真源是已归档产物） |
| 汇总卡片 / speedup 热力表 / 判定书证据 / 逐 case 明细 | 不做交互式钻取（Roofline 悬停/时间轴切片是全量采集报告的形态，非本套件范围） |
| 缺失模块如实呈现原因（MISSING/无数据） | 不通过视觉插值补齐不存在的数据 |

## 通用规则（可视化报告合同）

1. 所有区块消费同一矩阵目录的 snapshot JSON（单一数据真源）；
2. `0` 是有效数据，`—` 表示缺失；
3. **不得视觉插值补齐**：无数据的 case 显示 MISSING 与原因，不画趋势线；
4. 离线可打开：不依赖远程资源（字体/JS/CDN 全内联或不用）；
5. 着色语义固定：绿=KEEP（越深收益越大）/ 红=REVERT / 浅灰=噪声带内 /
   深灰=无数据——颜色只编码结论档，不是性能排名。

## 报告结构（render_html.py 已实现）

```bash
# 前置：batch_pipeline 已产出矩阵目录（comparison_matrix.md + 逐 case 子目录）
python3 skills/ops-verdict/scripts/render_html.py \
    --matrix-dir tuning_artifacts/<op>/matrix \
    --title "OpsTune 调优报告 — <op>@<soc>" \
    -o tuning_artifacts/<op>/report.html
```

| 区块 | 内容 | 数据源 |
|------|------|--------|
| 汇总卡片 | 逐 variant：geomean speedup / KEEP / REVERT / 噪声带内 / MISSING 计数 | 矩阵 snapshot JSON |
| 逐 case 热力表 | case × variant 的 speedup 着色 + 悬停结论 | 同上 + comparison.md |
| 逐 case 明细折叠 | comparison.md 原文（三段结论 / 抖动诊断 / 机制变量 WARN） | 同上 |

REPORT.md 六要素中「逐 case 对比表」「保留与回滚清单」两节的人读形态即此报告。

## 扩展位（按需，模块化合同）

后续按需扩展的模块与数据前提（当前**不虚宣称已支持**）：

| 模块 | 数据前提 | 实现路径 |
|------|---------|---------|
| Roofline 散点 | arithmetic intensity 实测点 + soc-spec 峰值线 | facts_bridge 标量 + soc-spec 画 ceiling/ridge |
| 重叠度时间线 | trace_core*.json（cannsim 或 A5 PipeTimeline） | trace_bubble.py 的 per_core 数据直绘 |
| 判定书证据段 | verdict.json findings（severity/confidence/反证） | 直接 JSON → HTML 段 |
| 抖动折线 | snapshot p50/p95 分布字段 | 已有数据，折线形态按需 |

## 与生成链的完整编排（调优结束时一条链）

```bash
# ① 矩阵（batch_pipeline --from-collect 或显式 --perf）
python3 .../batch_pipeline.py --cases cases.csv --from-collect <collect_root> \
    --baseline-variant baseline --after-variants round-01-<mech> \
    --scope scope.json --output-dir tuning_artifacts/<op>/matrix
# ② 静态 HTML
python3 .../render_html.py --matrix-dir tuning_artifacts/<op>/matrix -o tuning_artifacts/<op>/report.html
```

产物索引（P2-2）：comparison_matrix.md 末尾自动生成归档清单；report.html 一并
列入。HTML 与 markdown 矩阵同数据源——数字不一致 = 渲染 bug，报 issue 不手改。

## 模板资产

- `ops-verdict/scripts/render_html.py`（渲染引擎，单文件零依赖）；
- 数据契约：`ops-verdict/references/io-contracts.md`（snapshot/comparison
  字段）+ `opstune.verdict/1.1`（判定书）。
