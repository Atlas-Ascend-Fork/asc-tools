# PipeUtilization 分析指南

`PipeUtilization.csv` 描述 AIC/AIV 各执行流水的活跃时长、活跃周期占比、指令缓存未命中率及部分数据通路的活跃带宽。字段公式和可用性以 [CSV 指标目录](../assets/metric-catalog.json) 为准。

## 分析要点

- `time(us)` 和 `ratio` 描述同一流水活跃周期的不同表示，前者依赖频率，后者依赖总周期。
- 活跃带宽使用流水实际活跃时间作为分母，不等同于以整个 Kernel 时长计算的平均带宽。
- 部分活跃带宽依赖 `Memory` 或其他 Section 提供的计数。单独采集 `PipeUtilization` 时可能为 `NA`。
- AIC 和 AIV 记录分别分析，比较前匹配 `block_id`、`sub_block_id` 和 Core 类型。
