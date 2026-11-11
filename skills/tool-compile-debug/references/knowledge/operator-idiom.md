# 常见算子的数学参考

本页提供数学关系，供已有明确任务契约的实现变更使用；不是可直接编译的 Ascend C 调用配方。先核当前高层API是否能满足任务。声明缺失、未收录或聚合头未导出不自动要求分解算法。

| 运算 | 名称检索线索 | 数学关系 |
|---|---|---|
| softmax | SoftMax | 每行减最大值后取exp，再除该行exp和 |
| layernorm | LayerNorm | 减均值，除以含epsilon的标准差，再做仿射 |
| rmsnorm | RmsNorm | 按含epsilon的均方根缩放，再做仿射；无减均值 |
| silu | Silu | x / (1 + exp(-x)) |
| gelu | Gelu | 精确Erf定义或约定的tanh近似；两者不是同一精度契约 |
| reduce mean | ReduceMean | 沿指定轴求和再除实际元素数 |

名称只用于调用 CANNBOT `ascendc-docs-search` 技能来检索声明，不是对任何dtype/shape/目标的支持承诺。
9.0 已核声明的拼写是 `Silu`；不把`SiLU`作为未经证明的等价API名。
`ReduceMean` 声明存在（9.0 已核），不要沿用「只能叫 Mean」的说法。 SoftMax/Silu等在3510的具体入口和实例须分别核验，不能沿用旧页整族禁止结论。

确定需要分解后，先固定axis、广播、dtype、累加精度、尾块、epsilon和允许误差。逐个核基础算子的完整签名与调用层，计算临时空间和同步要求，再用合法输入及独立 golden 基准验证。给出的标量数学式不表示向量广播、tensor形状或临时buffer可以省略。无法取得合法输入或独立契约时，只报告编译层结果，不称数值已修复。

惯用分解配方与数学参考由随包引擎的知识库（`scripts/engine/harness/knowledge/operator_idiom_kb.yaml`）提供。
