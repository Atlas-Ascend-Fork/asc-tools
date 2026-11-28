# L2Cache 分析指南

`L2Cache.csv` 描述 AIC 和 AIV 的 L2 近端、远端读写命中、未命中和 Victim 计数。字段定义、单位、公式和 `NA` 条件以 [CSV 指标目录](../assets/metric-catalog.json) 为准。

## 分析要点

- 读写命中率的分母包含近端和远端的命中、未命中及 Victim 六类计数。
- AIC 与 AIV 必须在各自数据行内计算，不能跨 Core 类型合并。
- 六类计数总和为零时，代码输出 `0`。计数缺失时输出 `NA`，两者含义不同。
