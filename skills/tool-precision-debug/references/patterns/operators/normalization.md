# Norm 算子族

## 数学与 reference

RMSNorm/LayerNorm 的 reference 包含统计量归约、epsilon、倒数或开方、缩放与可选偏置；误差可在归约后逐层放大。

冻结归一化轴、每组有效元素、mean/variance 或 square-mean 公式、epsilon 的位置/dtype、gamma/beta 广播和输出 dtype。GroupNorm 还要冻结 group 合法性和每组元素数；物理 padding 不得进入统计。

## I/O 与实际链路

- 冻结输入/输出 shape、dtype、layout、有效区、padding 和 alias。
- 从 host tiling、GM 搬入、局部计算、同步到 GM 搬出逐节点记录实际计算链，不以 API 名替代。
- tiling 分支、多核分区、尾块与特殊值必须进入风险清单。

## 敏感 checkpoint

按当前疑点，从输入解码、首次布局/搬运后、核心计算前后、跨核合并前后和最终有效区输出中选择少量相关点，说明 reference、CPU 或 NPU 观察面及所用参考或检查性质。没有可靠局部 oracle 时先保留原值或检查适用的不变量；无法判断的观察记为 `AMBIGUOUS`，不据此宣称该段数值正确。选点与局部对照见[观察点设计](../../diagnosis/checkpoint-design.md)；已有可比误差但不满足单调边界时，再使用逐层误差分析。

沿相关数据流检查有效输入、统计量 partial、合并后统计量、epsilon 后分母、归一化值或仿射输出，按疑点从全零、全同、均值大而方差小、group 尾块和 padding 哨兵中选择输入。若统计集合错误，先修复 mask/count；集合正确但误差随长度或公式变化，再比较累加和消减机制。

padding 哨兵只写入已经分配且合同允许写入/观察的物理 lane，不能越过 allocation capacity。先使用有限哨兵；NaN/Inf 仅在目标 API 和路径允许时加入，并保留有限值 control。poison-only 失败只能说明实验触及 padding 路径，不能单独确认统计集、搬运或越界中的某一个根因。

## 大均值、小方差的公式对照

对 LayerNorm/GroupNorm 等需要方差的路径，将实际 Kernel 的方差链明确写成下列哪一类：

```text
矩公式:      var = mean(x * x) - mean(x) * mean(x)
两遍中心化: var = mean((x - mean(x)) * (x - mean(x)))
```

当 `x = C + delta` 且 `|C|` 远大于 `|delta|` 时，矩公式需要相减两个接近的大量，是消减候选的敏感输入。两遍中心化在数值上通常更稳定，但不能因此自动替换：总体/样本方差、归约顺序、中间 dtype 和目标算子的公式语义仍必须一致。RMSNorm 使用 square-mean 路径时也不应套用去均值的方差诊断。

对两条公式使用同一有效集、同一输入位模式和同一逐步 dtype 建立独立 CPU 算术链。如果只有矩公式在“大均值、小方差”输入上出现零、微小负值或显著放大的误差，只能提高“矩公式消减” 候选；还要证明实际 Kernel 使用同一公式和中间 dtype。

### 微小负方差的候选分流

`var < 0` 不等于已确认需要 clamp。在加 epsilon 前保存方差位模式，并按首个分叉分流：

- 矩公式的 `mean(x*x)` 与 `mean(x)^2` 各自接近 oracle，相减后首次得到微小负值：消减/舍入是候选，继续用偏置扫描和公式对照验证。
- partial sum、count、mask 或合并结果已经分叉：先查统计参与集、尾块和跨核归约，不把负值归因于公式。
- 两遍中心化中的平方项或平方和已为负：该现象不由正常的“非负项求和”解释，优先查数据解码、错 offset、未初始化、同步、越界或证据采集。
- 只在特定 tiling/长度分支为负：保持数值分布不变，用分支边界两侧的 shape 对照归约顺序、count/mask 和尾块候选。

只有当前算子语义允许、已排除错误统计集或搬运问题，并有单变量实验与边界回归支持时，才把 `max(var, 0)` 作为修复候选。否则 clamp 会遮蔽上游错误，也可能改变当前算子的特殊值语义。

## 分块 Welford：合并统计状态而非局部方差

只有切成多块、出现短尾块或跨核合并后均值/方差才错时，先对每个逻辑统计组保存 `(mean, M2, count)`：`M2` 是相对于**该块均值**的偏差平方和，`count` 是有效元素数，不是对齐长度、块数或核数。将两份非空状态合并的实数恒等式为：

```text
n = n_a + n_b
delta = mean_b - mean_a
mean = mean_a + delta * n_b / n
M2 = M2_a + M2_b + delta * delta * n_a * n_b / n
variance = M2 / (n - correction)   # 最终统计组，且分母在合同允许范围内
```

例如 `[0,0]` 与 `[10]` 两块的局部 `M2` 都为零，但整体 `mean=10/3`、`M2=200/3`，总体方差为 `200/9`。平均局部均值会得到 `5`；平均局部方差或漏掉跨组偏差项会得到 `0`。这个反例检查统计状态和权重，不能用扩大容差修复。tail 必须携带自己的实际 count；某个输出只有一项，也不代表只需保存一份累加状态。

需要把不等长、空状态和局部修复分别做成对照时，按需查看 [Partial 统计合并：局部正确不等于全局正确](../cases/partial-statistics-merge.md)；实际统计状态仍按当前算子合同解释。

空 partial 不提供均值：先按有效集判断，只有一侧非空时沿用该侧状态，两侧都空时按当前算子的空集语义处理。不要读取空槽的残留 mean/M2 后指望乘零消除它。correction 的应用位置与总体/样本语义一起冻结；`n-correction<=0` 时不能从上式猜测输出，仍查询当前算子合同。

用同一有效输入分别做等长块、不等长尾块、含空任务和反向合并的对照，在局部状态、第一次 merge、最终除法三个边界比较独立 oracle。先核对 count 与状态含义，再检查每一步的实际 dtype、count 换算和合并树；实数公式等价不保证浮点逐位相同。固定每 8 块一组不是误差保证，Welford 也不能擅自替换任务规定的公式。累加顺序的后续分叉见 [累加、归约顺序与消减](../mechanisms/accumulation-order.md)。

## 分块 Softmax：先统一最大值基准，再合并

单块正确、分块后 softmax 权重或加权归一化输出才错时，检查每个 partial 是否携带了同一逻辑行的最大值 `m`、缩放后的指数和 `l`、未归一化加权和 `u`。对有限 score `s_i`、有限载荷 `v_i` 和非空有效集合，数值模型为 `m=max(s)`、`l=sum(exp(s_i-m))`、`u=sum(exp(s_i-m)*v_i)`；`u` 也可以是向量。两份非空状态合并时，分母与分子都要换到共同基准：

```text
m = max(m_a, m_b)
alpha = exp(m_a - m); beta = exp(m_b - m)
l = alpha * l_a + beta * l_b
u = alpha * u_a + beta * u_b
output = u / l
```

若实际实现存储的是已归一化输出或 log-sum-exp，先按其表示推导合并关系，不能把它当作上述 `u/l` 载体直接相加。让后一块的最大值分别低于、等于、高于前一块，并在两块放置不同载荷，可区分漏更新最大值、只重缩放分母、只重缩放分子以及直接平均局部输出。比较的是同一有效输入上的每级状态，不能用单块通过证明跨块状态已接通；真实 dtype、Exp 路径和 merge 顺序仍须分别核对。

首个非空块可以直接建立状态；空块不应产生新的数学贡献。是否清零、覆盖或保留具体 buffer 按实际读写路径判断，不能读取未定义旧状态后靠 `0 * old` 消除它。整行被 mask 屏蔽时，应先按合同判定空有效集；将屏蔽值换成同一个有限负数并不能自动产生零贡献，因为减去该行最大值后每项仍是 `exp(0)=1`。空行、NaN/Inf 和最终分母异常的行为由当前输出语义决定，不默认添加 epsilon 或 clamp。

这一方法由 FlashAttention 的在线状态引出，只用于当前确实存在分块指数归一化的路径，不要求采用特定 FA 流水、任务切分、API 或性能模板，也不把上述有限数模型当作 NPU 验证。

## Epsilon 位置是算子语义

下列表达式不可互换，需要由当前 oracle/独立规范决定哪一个才是验收目标：

```text
inv_std = 1 / sqrt(var + epsilon)
inv_std = 1 / (sqrt(var) + epsilon)
inv_std = rsqrt(max(var, 0) + epsilon)
```

同时冻结 epsilon 的来源、值、存储 dtype、运算 dtype、量化时点、加入位置与平方根/倒数顺序。两个实现即使在普通方差上误差很小，在全同、零方差或 epsilon 主导区也可能明显分叉。把 epsilon 从 `sqrt` 内移到 `sqrt` 外即使让当前样例通过，也只是改变了公式的候选实验；在 oracle/规范没有要求该位置时不能称为修复。

实验时不为了让当前样例通过而搜索 epsilon。先从合同获取它，然后在下列 checkpoint 定位首次分叉：`mean/square-mean`、epsilon 加入前统计量、加入后值、`sqrt/rsqrt`、归一化值和仿射输出。

## 最小输入矩阵

| 输入族 | 构造方式 | 主要区分的候选 | 必须的相邻对照 |
|---|---|---|---|
| 全零 | 有效集全为 `+0`；合同关心符号时另测 `-0` | 零方差、epsilon 位置、特殊值语义 | 单个非零元素 |
| 非零常量 | 每个归一化组内全相同 | 去均值、count/mask、零方差 | 只改一个 lane |
| 大偏置+小扰动 | `x_i=C+delta_i`，先确认各 `delta_i` 在输入 dtype 中仍可表示 | 矩公式消减、中间 dtype | 相同 `delta_i` 的零中心组 |
| 零中心小扰动 | `x_i=delta_i` 且二阶统计与上一组匹配 | 分离偏置消减与普通累加误差 | 大偏置组 |
| epsilon 主导 | 构造方差分别低于、接近和高于合同 epsilon 的组 | epsilon 位置/量化、`sqrt` 与 `rsqrt` 顺序 | 三个点保持 shape/tiling 一致 |
| 统计边界 | 归一化长度 `B-1/B/B+1`、group 尾块、允许观测的 padding | count、mask、尾块、分支和跨核合并 | 保持数值分布只改 shape |

每组同时记录逻辑轴/分组、有效 count、实际 tiling 分支、partial 归约顺序和中间 dtype。若改 dtype 同时改变 tiling，按 [dtype 变化的分层对照](../../diagnosis/numerical-modeling.md#改变-dtype-时核对哪些变化)区分相关候选，不用该结果单独证明累加精度。

## 版本路由与边界

相关说明：[累加、归约顺序与消减](../mechanisms/accumulation-order.md)、[NaN、Inf、溢出与次正规数](../mechanisms/special-values.md)、[Cast、舍入与中间精度](../mechanisms/cast-rounding.md)。epsilon 数值、API 实现和支持范围属于具体算子/版本合同；本页不代表任一 NPU case 已验证。

提高中间精度、clamp 或添加 epsilon 只在算子合同允许且当前单变量实验支持时作为修复候选。

本页公式是带条件的数学模型；具体 CANN 调用及其状态表示仍需查询当前版本 API 文档。

max/sum 的重缩放与 SoftmaxFlashV2 的分块状态相关，具体接口见当前版本的 Softmax API 文档。加权和 `u` 的同系数合并由本页的数学定义推导；使用时须确认当前状态定义和归一化公式一致。
