# Memory 分析指南

`Memory.csv` 描述 AIC/AIV 主存访问、AIC L1、AIV UB、MTE 指令，以及数据量和带宽利用率。字段定义、单位、公式和 `NA` 条件以 [CSV 指标目录](../assets/metric-catalog.json) 为准。

## 分析要点

- GM 到 UB 相关计数按代码规则扣除修正计数，结果小于零时按零处理。
- `time(us)` 是对应 PMU 行的周期时长。混合 Kernel 的带宽分母可能使用 Task Log 的 `D_op`，不能直接用行时间反算。
- Block CSV 中的数据量按单条 Core 行输出。当前 Task 摘要汇总各已采集侧的主存读写数据量。早期混合 Kernel 摘要输出有效 PMU 行的平均值，跨版本比较前确认生成版本的口径。
- 带宽利用率使用对应 SoC 的最大带宽，并限制在 100% 以内。
- 固定为 `NA` 的字段不能解释为零带宽或零数据量。
