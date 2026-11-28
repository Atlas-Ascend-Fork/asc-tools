# MemoryL0 分析指南

`MemoryL0.csv` 描述 AIC 的 L0A、L0B、L0C 数据通路带宽。字段定义、单位、公式和 `NA` 条件以 [CSV 指标目录](../assets/metric-catalog.json) 为准。

## 分析要点

- L0 带宽字段只适用于 AIC，AIV 行中的专用字段为 `NA`。
- 混合 Kernel 且 Task Log 时长有效时，带宽可能使用 `D_op` 作为分母，否则使用 AIC 行时长。
- 比较不同采集结果前确认 Core 类型、频率、数据通路和时间分母一致。
