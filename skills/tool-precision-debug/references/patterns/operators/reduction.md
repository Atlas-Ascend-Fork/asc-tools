# 归约算子族

## 目录

- [数学与 reference](reduction.md#数学与-reference)
- [I/O 与实际链路](reduction.md#io-与实际链路)
- [敏感 checkpoint](reduction.md#敏感-checkpoint)
- [融合算子的归约支路](reduction.md#融合算子的归约支路)
- [Production 模式](reduction.md#production-模式多核局部量共享区与二次归约)
- [Atomic 模式生命周期](reduction.md#atomic-模式生命周期)
- [Partial 参与集合](reduction.md#partial-参与集合账本)
- [带索引 partial 合并](reduction.md#带索引归约局部位置与逻辑全局位置)
- [结果指纹候选矩阵](reduction.md#偏大偏小与波动的候选矩阵)
- [严格 bitwise](reduction.md#严格-bitwise-与-atomic-合并)
- [版本路由与边界](reduction.md#版本路由与边界)

## 数学与 reference

y = reduce(x, axis)；sum 的结合顺序、累加 dtype、尾部有效元素和多核合并顺序都是敏感点。

冻结 reduction 类型、axis 规范化、keepdims、每个输出对应的有效输入集合、初值、空集合语义和输出 dtype。Mean 还要冻结除数；Max/Min 要冻结特殊值与 tie 规则。不要把累加误差与 axis、窗口、mask 或输出 shape 错误混为一个候选。

同时把“数学上对同一集合求和”“项目要求的求值顺序”“项目 evaluator 的通过规则”分开。顺序 FP32 oracle、分组/树形 FP32 结果、逐位一致和 MERE/MARE 等容差通过不是同一合同；后两者即使都通过，也不能反推前两者的顺序相同。

## I/O 与实际链路

- 冻结输入/输出 shape、dtype、layout、有效区、padding 和 alias。
- 从 host tiling、GM 搬入、局部计算、同步到 GM 搬出逐节点记录实际计算链，不以 API 名替代。
- tiling 分支、多核分区、尾块与特殊值必须进入风险清单。
- 为每级 partial reduction 记录输入有效数、mask、累加 dtype、输出布局和下一阶段消费者。
- 记录 API 的精确函数名与重载：Tensor 前 count 个元素的 `ReduceSum(dst, src, workspace, count)`、带 mask/repeat 的高维切分 `ReduceSum` 和每 repeat 输出的 `WholeReduceSum` 必须分别查询官方 API 文档。

## 敏感 checkpoint

按疑点从输入解码、布局/搬运、局部归约、跨核合并和最终输出中选择少量相关观察，说明观察面、参考依据与有效区。缺局部 oracle 时先保留数值或检查适用的不变量，不据此排除整段计算。具体见[观察点设计](../../diagnosis/checkpoint-design.md)；已有可比误差且需要解释传播、放大或抵消时，再使用[逐层分析](../../diagnosis/progressive-error-analysis.md)。

使用 axis 首尾、长度 1、整块、尾块、强消减和 padding 哨兵构造配对实验。若元素集合或 count 已错，先修复算子语义；集合正确但误差随长度/合并顺序增长，再路由 [累加、归约顺序与消减](../mechanisms/accumulation-order.md)。

固定 shape 的验收不能证明相邻长度或非法 shape 行为；若诊断实验临时改变 count、分块或核数，必须明确它只是区分机制的局部 oracle，不得计入原接口回归。Golden-only 自检也不能替代 actual 与 golden 的独立比较。

## 融合算子的归约支路

逐元素输出通过、只有 bias 或统计输出失败时，先画出分叉点：`中间 FP32 值 → 输出 Cast` 与 `同一中间值 → 归约 → 输出 Cast`。已收窄输出即使逐位一致，也不能证明上游 FP32 值一致；更不能把这些输出重新升 FP32 后求和当作未收窄归约的 oracle。

按疑点检查归约前 FP32 值、chunk partial、跨块合并、最终 Cast 和写回；某一路输出正常也不证明另一路的地址与最后一次写入正确。无 bias 对照可隔离是否进入归约，但改变 has_bias 可能同时改变 tiling，需要核对实际路径。

集合与归约输入一致后，有限值偏差查[分层归约与主尾列](../mechanisms/accumulation-order.md#框架参考的分层归约与主尾列分派)，类别失配查[顺序与溢出](../mechanisms/special-values.md#有限输入的归约顺序也会改变非有限类别)。修复后出现新的整段尾区异常则查[可变计数](../mechanisms/tiling-tail-state.md#可变计数被消费后续输出漏写与清零不完整)。[GRU p2/p3 案例](../cases/thnn-fused-gru-bias-reduction.md)串起了这些分叉。

## 归约接口组合中的常见误用

| 现象 | 先核对什么 |
| --- | --- |
| 非对齐行结果偏大或尾部候选丢失 | 物理行距与有效列数分开；展开 mask/count、Compare 和搬运实际覆盖的元素。需要读取补齐区时，填充值应对当前归约或比较无影响 |
| 最值正确，索引被读成微小浮点数 | 按当前重载解释索引的位编码，不能把编码字段直接当浮点数值；再做局部到全局的位置映射 |
| 带索引的分组结果残留或相邻结果被覆盖 | 目的区是否为完整记录预留了空间与对齐；修正跨度时同步修改写入和消费偏移 |
| pattern 输出数量或方向错误 | 用小二维矩阵对照归约轴，分别核对逻辑 shape、物理补齐、临时空间和当前重载 |
| 复算时输入已改变 | 是否启用了源空间复用，或临时区覆盖了输入；用合法的独立缓冲路径对照 |
| ReduceAll 或特殊值结果不符 | 原输入是否满足该接口的布尔编码或特殊值要求；只有算子定义要求时才做输入转换 |

调用 CANNBOT `ascendc-docs-search` 技能分别查基础归约和按轴归约的原型、类型、对齐与临时空间；更名调用同时查接口变更与参数顺序。repeat 截断、分批漏项和零工作量见[调用计数与分批状态](../mechanisms/tiling-tail-state.md#调用计数与分批状态)。

一个历史对照记录了 CANN 9.0.0 / Ascend910B1 上的 `whole_reduce_sum_unalign`：half 输入 shape 为 `[13,123]`，容差 0.001 的验收通过，但有 3 行与参考值相差 1 ULP。它说明容差通过不代表逐位相等；当前差异是否来自归约顺序，仍要检查参与元素、具体求值路径和中间 dtype。

按轴归约中启用 isReuseSource 时，查询该重载是否允许改写源，再检查后续消费者是否仍需要原输入。高阶 ReduceMean/ReduceProd 也需检查长轴中的中间范围和溢出；“用了高阶接口”不排除数值边界，按轴分段观察应保留逻辑 shape、padding 和有效输出。

## Production 模式：多核局部量、共享区与二次归约

对于分阶段归约，可以分别检查以下关系：各核计算局部统计量，受控地合并或写入独立 workspace，所有参与核到达完成边界后，再由下一阶段做最终归约。审计时分别验证任务分配与 tail、局部结果就绪、共享区初值/ownership、核间完成条件、最终参与集合和累加 dtype。原子开关、核内依赖与全核同步是不同责任。先从当前 Kernel 确认实际阶段与参与集合，具体 API 支持须按当前版本核对。

若最终结果偶尔偏大，先用每核唯一哨兵判断是局部结果重复写、原子模式未恢复、workspace 未清零，还是最终归约集合重复；若偶尔偏小，检查局部搬出发生在 producer 完成前或某些尾任务未参与。将 “原子合并”和“每核独立槽位后二次有序归约”作为单变量对照时，必须同时记录合并顺序改变带来的数值差异，不能把更稳定自动写成更符合原 oracle。

## Atomic 模式生命周期

把 Atomic 当作影响后续搬出的显式状态，而不是某一行调用的局部注释。沿所有分支和 early return 画出状态机：普通写状态 → 仅在目标合并搬出前开启 → 执行目标写入 → 立即恢复普通写状态。再核对目标 GM/workspace 初值、受 Atomic 影响的精确写集合，以及恢复后第一条普通写。若某条路径漏恢复，后续本应覆盖的搬出可能变成累加候选；若过早恢复，目标 partial 可能退化为互相覆盖候选。

历史样例中出现过 `SetAtomicAdd` 与 `SetAtomicNone` 成对包住一次搬出，也记录过“未恢复后结果偏大”的排查线索。这只是一种历史观察。当前 profile 下具体 Atomic API 名、支持 dtype、作用于哪类搬运、恢复接口、所需核内流水依赖、跨核可见性与同步参与规则均为 UNKNOWN，必须调用 CANNBOT `ascendc-docs-search` 技能读取对应版本的 DMA 原子及相关搬运文档后才能下结论。

模板声明的原子类型还要与真实输出 dtype 对齐。初始化覆盖与浮点合并乱序可能同时造成运行间波动，先分别对照 GM 初值、贡献集合和普通写/原子写的生效区间；DMA 原子的具体同步插入点及特殊值模式调用 CANNBOT `ascendc-docs-search` 技能核对，不跨版本照搬。

### 跨卡归约对照

AllReduce 与单卡参考有微小位差时，先核对每个 rank 的贡献及通信前后 dtype，再固定通信域、进程级确定性配置和执行形态做对照。配置相同不独立保证两个实现采用相同求和顺序，逐位要求也不能自行放宽成容差。若结果是旧值或缺少某个分段，先查[通信缓冲与完成范围](../mechanisms/multicore-ownership.md#跨卡通信的缓冲所有权)。Host 通信的配置入口调用 CANNBOT `ascendc-docs-search` 技能确认。

## Partial 参与集合账本

对每个最终输出 `y[j]` 同时建立元素级数学集合和实现级 partial 多重集：

```text
ExpectedElements(j)
  -> Tasks(core, round)
  -> Partial(core, slot, valid_count, dtype, producer_epoch)
  -> FinalContributors(j)
```

分别验证每个 required 元素进入且只进入合同规定的 partial、每个 partial 的有效长度和 tail 正确、空核/空任务没有伪造贡献、最终阶段没有漏槽或重复读取同一槽。这里必须保留 contributor 的重数；简单集合并集会把“同一 partial 被加了两次”错误地去重。先比较每个 partial 的独立 oracle：局部值已错时查上游计算与有效区；所有局部值都对而最终错时，再查参与集合、初始化、可见性和合并顺序。

对 chunked reduction，参与多重集正确只证明 partial 没有漏项或重复。若问题转为相同参与集合下的舍入、分块或合并次序，则路由到 [累加、归约顺序与消减](../mechanisms/accumulation-order.md) 冻结对应 oracle 的顺序合同；本页继续负责参与集合、实际计算链和算子验收面的核对。

若每个 partial 已与局部 oracle 一致而统计 merge 仍错，按需查看 [Partial 统计合并：局部正确不等于全局正确](../cases/partial-statistics-merge.md) 的不等长/空状态对照；它不替代本页的参与多重集审计。

## 带索引归约：局部位置与逻辑全局位置

最大/最小值正确而索引错误，或只在非首块、多核、并列极值时失败，先把每个 partial 写成 `(value, logical_index)`，同时记录 `local_ordinal -> logical_index` 的映射。输出索引可能是归约轴坐标或某个声明顺序下的展平坐标，不能直接换成 GM 字节 offset；只有连续且无跳跃的同一坐标系，才能简化为“块起点 + 局部索引”。多轴、stride、repeat 和 tail 路径按各自有效集合展开。

合并时 value 与 index 必须来自同一候选，不能只合并 value 再保留任意局部 index。若合同规定返回第一个极值，有限普通输入下的 tie 应选择该逻辑遍历顺序中更早的 index，而不是先到达的 partial。例如 `[1,7]` 与 `[2,7]` 两块映射到全局位置后，最大值都为 `7`，索引分别为 `1`、`3`；反转合并到达顺序仍应选 `1`。只用严格 `>` 并保留先到达者，会在后一块先到时误选 `3`。

区分实验分别改变非零块起点、极值所在尾块，以及两个含相同极值的 partial 到达顺序；另用不同极值作为非 tie 对照。在局部结果解码、全局映射、成对合并和最终索引转换后比较首个分叉。若索引暂存于浮点中，再转查 [Cast、舍入与中间精度](../mechanisms/cast-rounding.md) 的整数碰撞边界；不同 API 返回的索引可能是数值或编码字段，应先查当前重载再决定 Cast 还是位解释。

first/last、NaN、符号零及索引遍历顺序都按任务合同确定，不能无条件采用 LE/GE 或强制 float 索引。mask 外和 padding lane 不应伪造候选；实际 API 的 calIndex、索引格式和临时空间需查询当前版本来源。

## 偏大、偏小与波动的候选矩阵

| 结果指纹 | 优先核对的可证伪候选 |
|---|---|
| 稳定偏大 | 输出/workspace 未按合同初始化；partial 重复参与；同一 task 被两核处理；Atomic 状态泄漏使后续普通写继续累加 |
| 稳定偏小 | tail task 或某些参与核遗漏；空核/参与核判断错误；partial 在 producer 完成前搬出；最终阶段漏读有效槽 |
| 同输入运行间波动 | 初始化不完整；共享写或完成边界不明确；未固定的合并次序；读到了不同 epoch 的 partial |
| 数值接近但逐位不同 | 参与集合相同但合并树、partial 存储 dtype 或舍入点不同；先不要归因于漏项/重复项 |

这些只是候选排序，不是由误差正负直接推出根因。用每核唯一哨兵、slot 有效标记、两项可手算输入和强消减输入分别区分“谁参与了”与“以什么顺序累加”。增加同步后现象变化也只证明实验触及执行路径，必须同时证明同步覆盖了正确 producer、consumer 和参与核集合。

若局部量逐项正确但最终结果偶尔多一份或少一个 partial，优先比较 `FinalContributors` 的带重数清单和共享区有效标记；不要重新调整局部 Reduce 公式来掩盖合并阶段的漏项或重复项。

## 严格 bitwise 与 Atomic 合并

先冻结验收要求到底是顺序 oracle 的逐位一致，还是允许明确容差的数学归约。若当前 Atomic 合并合同没有权威证据保证固定顺序，就不能仅凭它与某个串行求和结果的 bitwise 差异证明 Atomic 参与集合或具体实现机制错误；若项目合同要求严格 bitwise，该差异本身仍是验收失败。反过来，容差通过也不能证明参与集合、特殊值类别或重复运行稳定性正确。对 NaN、Inf、符号零等仍按算子合同单独比较类别和符号。怀疑归约顺序导致位差时，查[累加、归约顺序与消减](../mechanisms/accumulation-order.md)。

要求 bitwise 时按项目规则比较最终输出；只有另有求值顺序要求，或正在调查合并顺序差异时，才展开 partial 的生成、存储 dtype 和合并过程。“每核独立槽位 + 确定顺序二次归约”可作为区分实验。只要求容差时，在参与集合正确的前提下评估误差，不自行升级为逐位验收。

## 版本路由与边界

基础归约与 DMA 原子的接口资料调用 CANNBOT `ascendc-docs-search` 技能获取。API 的 mask、repeat、临时空间、Atomic 与同步支持范围应根据所查官方页面并结合当前产品核对；本页不代表任一具体实现或 NPU case 已验证。

带索引 partial 的局部索引必须转换为最终输出的位置；具体 ReduceMax 索引示例调用 CANNBOT `ascendc-docs-search` 技能读取。其它 API 的索引坐标系与 tie 语义须单独确认。
