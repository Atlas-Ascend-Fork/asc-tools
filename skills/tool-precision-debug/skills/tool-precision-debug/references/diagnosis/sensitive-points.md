# 计算链敏感点

根据已理解的实际计算链选择下面相关的敏感点；计算关系仍不清楚时回到[算子建模方法](operator-modeling.md)。需要算子或子计算的专属经验时，再到页末目录选择对应内容，不要求每次逐项重读。

这里的敏感点是可能改变数值、参与元素、分支或数据可见性的节点及连接。把链上位置、触发条件和可能的偏差标在图旁，注明已确认的信息与缺口；没有相关结构的条目跳过。此阶段以源码和已有资料核对为主，标记不等于确认错误，也不要求每个敏感点都立即插桩或运行实验。阶段 3 再按异常现象和区分力选择验证对象。

## 目录

- [通用敏感点](#通用敏感点)
  - [运算顺序与中间数值范围](#1-运算顺序与中间数值范围)
  - [dtype、Cast、量化与精度模式](#2-dtypecast量化与精度模式)
  - [归约、统计量与误差放大](#3-归约统计量与误差放大)
  - [分支、函数输入域与特殊值](#4-分支函数输入域与特殊值)
  - [布局、索引、搬运与有效区](#5-布局索引搬运与有效区)
  - [shape、分块、mask 与尾部](#6-shape分块mask-与尾部)
  - [Buffer 生命周期、同步与所有权](#7-buffer-生命周期同步与所有权)
  - [参数角色、实现分派与最终输出](#8-参数角色实现分派与最终输出)
- [算子专属敏感点目录](#算子专属敏感点目录)
- [标记保存位置](#标记保存位置)
- [标记完成后](#标记完成后)
- [产出示例](#产出示例)

## 通用敏感点

### 1. 运算顺序与中间数值范围

在连续乘除、乘加、Exp 后乘大系数、相近数相减等位置，写出实际逐步计算，而不只写化简后的公式。分别核对 reference 和 Kernel 的中间 dtype、已确认的舍入点、是否融合及结果范围；输入和最终数学值都有限，仍可能发生中间溢出、下溢或有效数字丢失。

例如 `a*(b/c)` 与 `(a*b)/c`：前者可能先除成零，后者可能先乘成 Inf。将敏感点标在首次可能分叉的 Div 或 Mul，而不是只标最终输出。按目标 dtype 构造能区分两条链的少量输入，必要时调用任务实际使用的 reference 确认；具体模型见[乘除重结合](../patterns/mechanisms/algorithm-parameter-contract.md#乘除重结合的有限精度分叉)。

“数值更稳定”不自动等于符合当前要求。若任务要求匹配指定框架的浮点行为，reference 的 Inf 而 Kernel 为有限数也可能失败；若任务允许按高精度 reference 和容差验收，则以该判据为准，不强求内部操作完全一致。参考求值方式尚未确认时保留疑点，不凭算子名称或数学恒等式决定顺序。

### 2. dtype、Cast、量化与精度模式

在每次转换、混合精度计算、累加与输出编码处标出输入、中间和输出 dtype。检查先缩窄再扩大是否已经丢失信息，整数转浮点是否发生碰撞，scale/offset、舍入和饱和作用在哪一步；仅看最终 dtype 不足以判断这些变化。

把半整数附近、相邻可表示值、整数精确范围边界和过早降精度标到对应节点，按当前输入域选择有区分力的对照。API 的近似模式、Fallback 或 dtype 改变可能同时改变实现路径，要核对实际采用的重载与分派。深入检查见[Cast 与舍入](../patterns/mechanisms/cast-rounding.md)及[精度模式与近似路径](../patterns/mechanisms/precision-mode-approximation.md)。

### 3. 归约、统计量与误差放大

在 Reduce、partial 合并、点积和统计量处，先标明参与元素、有效数量、累加 dtype、分块与合并顺序。漏项、重复、padding 参与计算和未初始化累加值，都可能看起来像普通精度误差。

再标记大动态范围、正负抵消、相近大数相减及后续倒数、归一化等放大位置。参考使用不同归约树或主尾分派时，抽出一段数据重新求和未必对应原节点；FP32 也不能保证任意结合顺序相同。检查方法见[累加与归约顺序](../patterns/mechanisms/accumulation-order.md)，复合计算的完整贡献关系见[计算边界](../patterns/mechanisms/complex-operator-boundary.md)。

### 4. 分支、函数输入域与特殊值

在 Select、分段阈值、clamp、epsilon、Log、Sqrt、Exp、除法和倒数处，分别记录函数实际输入与分支判据读取的值。确认比较符号、保护位置、dtype，以及判据应读取原始输入还是变换后的量；稳定计算量不能无依据地覆盖原始语义量。

边界点输出不同，也可能来自同一选中分支内部的算术差异。区分这两类解释时，按源码或有效观察核对判据值、比较结果和选中分支，再对照该分支的计算；仅凭边界输出失配，不能单独归因于分支条件。

按合法输入域检查阈值两侧、零分母、极值、NaN/Inf、符号零或次正规数。先比较 reference 与 actual 的数值类别，再解释有限值误差；特殊值也可能是规定的正常输出。不要用统一加 epsilon、clamp 或清零掩盖来源不明的差异。详见[特殊值机制](../patterns/mechanisms/special-values.md)和[Clamp 与分支判据](../patterns/mechanisms/algorithm-parameter-contract.md#clamp-与分支判据的数据流分叉)。

### 5. 布局、索引、搬运与有效区

在 CopyIn/CopyOut、reshape、transpose、broadcast、gather/scatter 或格式转换边上，核对逻辑坐标、元素 offset、字节地址、stride、物理容量与有效范围。区分值算错和正确的值被放到错误位置，留意 padding 被读入、广播角色错误和最终写回范围不一致。

将首尾行、相邻合法位置、布局转换前后和广播维度标在链上。可用可区分的坐标编码或合法 padding 哨兵检查映射，但不越过已分配区域。方法见[索引与地址归属](../patterns/mechanisms/index-address-ownership.md)及[搬运、对齐与有效区](../patterns/mechanisms/data-movement-alignment.md)。

### 6. shape、分块、mask 与尾部

在 Host 切分、tile/repeat 循环、mask 设置和尾块处理处，核对实际长度、迭代次数、掩码的消费方式、计数与地址推进。首块正确不代表后续块正确；逻辑长度相同也不代表使用相同路径。

按已确认的分块边界标记边界前、边界上、边界后的输入，以及合法的空输入、维度 1、rank-0 或零工作量路径。检查最后一块是否漏算、重复写回、沿用旧 mask 或旧状态。不能把这些形状一概当作合法输入，范围由任务规定。详见[Shape 边界](../patterns/mechanisms/shape-boundary-contract.md)和[Tiling 与跨 tile 状态](../patterns/mechanisms/tiling-tail-state.md)。

### 7. Buffer 生命周期、同步与所有权

在生产者到消费者、队列交接、缓冲复用、跨核合并和异步写回边上，标记谁写、谁读、何时写完、何时允许复用。变量不同不证明存储独立，源码先后顺序也不证明不同流水上的数据已经可见。

核对真实地址重叠、每轮初始化、同步参与者、事件轮次和输出区域的唯一写者。多核或多轮才出现的错误，应回到具体依赖边，而不是直接给整条链加同步。详见[Buffer 生命周期](../patterns/mechanisms/buffer-lifetime-alias.md)、[流水同步](../patterns/mechanisms/pipeline-sync-visibility.md)及[多核所有权](../patterns/mechanisms/multicore-ownership.md)。

### 8. 参数角色、实现分派与最终输出

在 Host 参数传递、VF/API 调用、属性缓存、实现分派及输出写回处，核对实参的逻辑角色、位置、dtype、地址和最后写者。相同类型的参数可以传反；变量名或 Kernel 已写某个缓冲，也不能证明 CopyOut 取的是目标结果。

将 beta/threshold/scale 等影响分支或数据映射的属性、shape/dtype 选择的不同实现、可选输入及多输出分别标到链上。已有测试没有覆盖但当前可达的路径，保留为待验证点；某一路通过不能外推全部实现。查[公式与参数槽位](../patterns/mechanisms/algorithm-parameter-contract.md)和[Golden、Host 属性与构建采用](../patterns/mechanisms/golden-host-contract.md)。

## 算子专属敏感点目录

需要专属经验时，对照下表查当前算子及其子计算，无需先通读全部通用条目。匹配时进入对应页，把相关敏感点补到同一条链上；融合算子可匹配多个算子族，不要求顶层名字完全相同。没有匹配项时，保留已有检查结果，按实际公式继续分析。

| 当前算子或子计算 | 对应资料与补充重点 |
| --- | --- |
| Reduce、带索引归约 | [归约算子族](../patterns/operators/reduction.md)：参与集合、索引布局、partial 与跨核合并 |
| AdaptiveAvgPool、自适应平均池化 | [小窗口对照](../patterns/operators/reduction.md#自适应平均池化的小窗口对照)：H/W 边界、非整除窗口、count 与累加 dtype |
| Broadcast、Transpose、布局重排 | [广播与转置](../patterns/operators/broadcast-transpose.md)：逻辑映射、物理行距、小通道与有效输出 |
| Sort、Merge、TopK | [排序与归并](../patterns/operators/sort-merge.md)：value/index 配对、并列规则、消耗计数与偏移单位 |
| Matmul、MMAD、分组矩阵乘 | [Matmul/MMAD](../patterns/operators/matmul-mmad.md)：分块、scale 装载、累加、异步写回及后处理 |
| RMSNorm、LayerNorm、GroupNorm | [Normalization](../patterns/operators/normalization.md)：统计量、方差公式、epsilon、分母及仿射输出 |
| Convolution | [卷积](../patterns/operators/convolution.md)：padding、窗口展开、group/dilation、K 参与集合及后处理 |
| DynamicQuant、DynamicQuantV2 | [动态量化](../patterns/operators/dynamic-quant.md)：统计范围、scale/offset 角色、舍入/饱和与多输出 |
| SoftplusV2Grad、SoftplusBackward | [Softplus 梯度](../patterns/operators/softplus-v2-grad.md#按残余症状进入案例)：角色、repeat/mask、乘除顺序、小量归零与属性复用 |

## 标记保存位置

在阶段 2 将相关计算链、敏感点和证据缺口写入报告第 3 节“相关计算链与数值约定”；报告位置与仅在对话交付的处理遵循[报告指引](reporting.md#任务产物目录)。

计算链可以直接画在报告中，敏感点用图旁注释或小表说明链上位置、触发条件、可能的偏差以及已确认或待核实的信息。图较大时可单独放在产物目录内，由报告使用相对链接引用；不为每个敏感点另建文件。后续取得新证据时更新同一份报告，保留关键判断变化。

## 标记完成后

检查标记是否落到了实际节点或数据边，能否说明在什么条件下产生什么偏差。阶段 3 优先验证与当前异常相符、且有低成本区分方法的候选；资料尚不能确认的点保留未知。敏感点不等于已确认的错误，通用与算子页都检查过也不代表它们已被排除。

## 产出示例

下面以 FP32 Softplus 梯度的求值顺序诊断为例，展示可写入报告第 3 节的内容。为突出标记方式，图中简写了内部缓冲和极负输入支路；实际任务按自身源码展开。S1–S5 只是本例的图内编号，不是固定格式。

### 3. 相关计算链与数值约定

**分析范围**：FP32 普通分支。Kernel 采用先除后乘，项目 reference 为 PyTorch 2.7.1 CPU 的 `aten.softplus_backward`。极负支路和掩码跨 repeat 行为继续保留检查项。

```text
grad、self（FP32）
    │
    ▼
CopyIn [S1：有效元素与数据就绪]
    │
    ▼
bx = beta * self
    ├──► mask = bx > threshold [S2：判据读取原始 bx]
    │
    └──► 按源码选择普通或极负输入支路
             ├── 普通：Mins(bx, threshold) → Exp → Div → Mul [S3]
             └── 极负：缩放计算链 [S4：次正规数与零的分叉]
                          │
                          ▼
                合并为待选择的 value
                          │
grad、mask ───────────────┤
                          ▼
              Select(mask, grad, value) [S5：mask 消费与结果来源]
                          │
                          ▼
                    CopyOut [S1]
```

| 标记 | 链上位置与触发条件 | 可能的偏差 | 当前依据与缺口 |
| --- | --- | --- | --- |
| S1 | CopyIn/CopyOut，关注有效长度、尾部和生产者完成时点 | 漏搬、错位、读到未完成数据或写回范围错误 | 检查实际地址、长度和依赖；本节未据此认定存在搬运错误 |
| S2 | `bx > threshold`，关注阈值两侧及计算前的 clamp/Cast | 变换后的量代替原始 bx，导致选错分支 | 保留原始 bx 到谓词的依赖，确认实际比较符号；边界覆盖仍需核对 |
| S3 | 普通支路的 Div/Mul，大幅有限 grad 与正 bx | 先乘发生溢出，先除保持有限，输出类别与 reference 不同 | 已有同版本 CPU 对照支持顺序分叉；仍须结合 Kernel 实际路径和原输入验证 |
| S4 | 极负支路的 Exp、缩放和后续放大 | 中间次正规数被舍入或清零，偏差被大 grad 放大 | 需要核对实际 API 模式、逐步范围和分支边界，不把所有小值差异归为 FTZ |
| S5 | Select 到 CopyOut，关注多 repeat 与最终写者 | mask 重复消费、误选支路或搬出旧缓冲 | 从实际重载和数据流检查，单个 repeat 正确不足以排除 |

**S3 的已有对照**：在上述 CPU reference 环境中，`grad=3.2e38、self=2、beta=1、threshold=20` 得到 `+Inf`；CPU 上先除后乘得到约 `2.82e38`。这支持求值顺序是敏感点，不代表更改候选后的 NPU 修复已经通过。

**阶段 3 的下一步**：先验证与当前 Inf/有限失配直接相关的 S3，将 Kernel 实际普通分支与 reference 对齐比较。S1、S2、S4、S5 保留为相关检查项，按源码和后续残差决定验证顺序，不把它们一并写成已确认根因。
