# ResourceConflictRatio 分析指南

`ResourceConflictRatio.csv` 描述 AIC/AIV 执行资源的等待和冲突情况。字段定义、依赖和公式以 [CSV 指标目录](../assets/metric-catalog.json) 为准。

## 分析要点

- 比例字段是原始比值，不乘以 100。
- AIC 指标使用 AIC 总周期作为分母，AIV 指标使用 AIV 总周期作为分母。
- 计数为零且分母有效时输出 `0`，字段不适用、计数缺失或分母为零时输出 `NA`。
- `aiv_vec_sfu_cflt_ratio` 汇总多个 SFU 冲突计数，任一组成计数不可用时整体为 `NA`。
