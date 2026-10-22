---
name: ops-report
description: 调优结果可视化报告。把 tuning_artifacts（verdict 判定书 + snapshot 口径指纹 + comparison 矩阵）转换为单文件静态 HTML 报告：汇总卡片（geomean/KEEP/REVERT）、逐 case speedup 热力表、判定书证据段。离线可打开、零外部依赖、缺失模块如实呈现不插值。触发：调优结束生成报告、结果可视化、汇报材料准备时使用。
---

# 调优结果可视化报告（ops-report）

> **本子域渲染脚本（render_html.py，落位 ops-verdict/scripts/）随「报告与仓级
> 接入」PR 交付**，当前为骨架占位——SKILL.md 先行声明报告合同，供归档阶段
> task-prompts 的前向引用对齐。

## 报告合同（蒸馏自可视化纪律）

1. 所有区块消费同一矩阵目录的 snapshot JSON（单一数据真源）；
2. `0` 是有效数据，`—` 表示缺失；**不得视觉插值补齐**：无数据的 case 显示
   MISSING 与原因，不画趋势线；
3. 离线可打开：不依赖远程资源（字体/JS/CDN 全内联或不用）；
4. 着色语义固定：绿=KEEP / 红=REVERT / 浅灰=噪声带内 / 深灰=无数据——颜色只
   编码结论档，不是性能排名。
