# ArithmeticUtilization 分析指南

`ArithmeticUtilization.csv` 描述 AIC Cube 和 AIV Vector 的算术资源利用情况，包括活跃周期比例、指令计数及 SIMD/SIMT VF 的发射时长和 IPC。字段名、单位、依赖、公式和 `NA` 条件以 [CSV 指标目录](../assets/metric-catalog.json) 为准。

比例字段是原始比值，不乘以 100。AIC 指标使用 AIC 总周期作为分母，AIV 指标使用 AIV 总周期作为分母。AIC 和 AIV 字段分别适用于对应 Core，不能跨 Core 类型合并。

## 分析要点

- 指令数量和活跃周期比例反映不同维度，不能用指令数替代周期比例。
- VF 发射占比以 AIV 总周期为分母，VF IPC 以对应 VF 发射周期为分母。
- 历史报告中的 `aiv_vec_vf_ratio` 和 `aiv_vec_simt_vf_ratio` 对应当前的 `aiv_simd_vf_ratio` 和 `aiv_simt_vf_ratio`。历史格式没有四个 VF 发射时长和 IPC 字段。
- 只有字段依赖的计数和总周期同时有效时，比例才具有可比性。
- 比较 Block 时匹配 `block_id` 和 `sub_block_id`，并区分 `cubeN` 与 `vectorN`。
- 需要具体字段定义时，使用 `python3 scripts/lookup_metric.py --section ArithmeticUtilization --field <字段>`。
