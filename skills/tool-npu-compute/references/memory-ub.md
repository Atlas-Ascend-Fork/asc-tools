# MemoryUB 分析指南

`MemoryUB.csv` 描述 AIV 的 UB、Vector 和 GM 数据通路带宽。字段定义、单位、公式和 `NA` 条件以 [CSV 指标目录](../assets/metric-catalog.json) 为准。

## 分析要点

- UB 带宽字段只适用于 AIV，AIC 行中的专用字段为 `NA`。
- 混合 Kernel 且 Task Log 时长有效时，带宽可能使用 `D_op` 作为分母，否则使用 AIV 行时长。
- `aiv_ub_read_bw_gm(GB/s)` 当前固定不可用，不能解释为零带宽。
- 字段方向按当前实现公式解释，不能仅根据字段名反推相反的数据方向。
