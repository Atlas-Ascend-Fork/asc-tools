# Tiling、尾块与跨 tile 状态

只在 shape 边界、第二个 tile 或最后一核失败时，分别验证 host tiling、核间所有权、循环状态和尾块有效区。改变 tile 大小会同时改变内存布局、循环次数、归约顺序和流水调度，不能单凭现象改善认定 UB 容量是根因。

若 Host tiling 把同一归约轴切成多个 chunk，还要记录每个 partial 的生成顺序、存储 dtype 和 pairwise/tree 合并拓扑。Host tiling 改变归约顺序时，即使参与元素集合相同，也可能不满足严格 bitwise reduction contract；这时联合查询 [归约算子族](../operators/reduction.md) 与 [累加、归约顺序与消减](accumulation-order.md)，不要把顺序差异误记成漏 partial。

## 逐层核对

1. Host：冻结原始 shape、属性、tiling key、blockDim、每核长度、tile 数和尾块长度。
2. Kernel 入口：Dump 实际收到的 tiling 字段，确认缓存 key 能区分所有影响代码路径的属性。
3. 核内循环：记录每轮 offset、有效长度、buffer 状态和参数加载；特别检查 tile 0 与后续 tile。
4. 输出：按核区间和逻辑有效区验收，物理 padding 不得作为数学输出。

将四类问题分开：通用 shape 合同、动态分支选择、合法 shape 的 tile/tail 切分、跨 tile 参数状态。最小 shape 失败时先确认 shape 合法且 tile 数、有效长度没有出现零或负值；相邻 shape 突变时保存 tiling key 与 kernel 符号；第二行 tile 0 继承上一行末 tile 的模式则检查参数加载和状态重置。

## 调用计数与分批状态

Host 算出的工作量、tiling 字段保存值和调用实参分别记录。若实参确为 uint8_t，256 转换为 0 是表示范围问题；该接口如何解释 0 还要另查。字段范围与接口自身调用上限是两层条件，超限时按当前规则分批，不能只把总量截小。

分批后逐段核对输入地址、有效长度、输出推进、partial 合并、筛选有效数和 mask 状态。使用不同位置的输入指纹比较单次合法调用与分批路径，确认每个合法元素恰好按算法要求处理。归约、排序、转置和 GatherMask 不共用一套 repeat 上限或零值语义，调用 CANNBOT `ascendc-docs-search` 技能分别核对调用粒度。

若零长度 tail 没有写目的区，而消费者仍沿用旧长度，就会读到上一轮结果。错误 tiling 修正计算；合法空任务按[Shape 与空输入要求](shape-boundary-contract.md#修复与回归)处理初始化和同步参与，不把零强改为正值。

### 可变计数被消费：后续输出漏写与清零不完整

同一 tile 的前几个输出段正确、后面的段未写，或者新增归约缓存后出现旧值时，检查 API 是否通过引用修改 count/remaining。把三种量分开：跨输出段复用的 `validLen`、当前段可消耗的 `remaining`、当前段的地址 offset。API 推进了剩余量，不代表已经推进地址；地址循环递增，也不代表应该再次扣减 remaining。

在已核对的 950 Reg/SIMD VectorFunction 路径中，`UpdateMask<T>(uint32_t& count)` 会按本轮可处理量更新 count。默认单寄存器配置令 `C=VL字节数/sizeof(T)`：本轮 mask 覆盖前 `min(count,C)` 个元素，随后 count 变为 `max(count-C,0)`。若直接传后续输出还要用的 `aLen`，一个尾段可能将它消耗到零；下一段便形成零 mask 或跳过写入。若随后又手工扣减一次，则可能提前耗尽 mask 或发生无符号下溢，留下未初始化的有效区。[MaskReg 文档](https://gitcode.com/cann/asc-devkit/blob/e5451c7feba11f9e259f4fbc19c4bf82f8c0496c/docs/api/SIMD-API/基础API/Reg矢量计算/寄存器数据类型/MaskReg.md)也包含双寄存器配置；具体产品、dtype 和 regTrait 仍调用 CANNBOT `ascendc-docs-search` 技能核对，不把每轮处理量写成跨配置固定常量。

当前 API 确认具有这种副作用后，可采用如下状态分工；这是循环组织示意，load/store 的对齐、mask 和同步条件需按实际代码填写：

```cpp
const uint32_t validLen = aLen;
for (uint32_t segment = 0; segment < segmentCount; ++segment) {
    uint32_t remaining = validLen;
    for (uint32_t offset = 0; offset < validLen; offset += elementsPerRepeat) {
        auto mask = AscendC::Reg::UpdateMask<float>(remaining);
        // 用 mask 处理本 segment 在 offset 处的一轮；不再手工扣减 remaining。
    }
}
```

区分实验不必继续扩大数值输入范围：选择一个完整 repeat、一个短尾段和多个共用长度的输出段，展开每次调用前后的计数与写入区间。可在诊断版本预填合法哨兵，观察哪些有效位置未被覆盖；若只补回局部 remaining 就让预测区间重新写入，支持计数副作用。算术模型通过却设备结果异常时，同样要核对初始化和写出覆盖，模型不包含这些内存操作。

修复后错误坐标从少数归约点变成按 segment 重复的整段尾区，应重新审查本次 diff 的计数、清零和写回。它可能是新增实现错误，不能一直用原来的求和树假设解释。具体的 45 个漏写位置与另一处清零不足见 [GRU 修复过程](../cases/thnn-fused-gru-bias-reduction.md#修复过程中的另一种机制可变计数污染后续输出)。

## 区分实验

选择刚好一 tile、加一个元素、少一个元素、多 tile 和尾核五类 shape。若错误总从第二轮相同相对位置开始，优先检查状态重置和参数重载；若只随核区间变化，先检查所有权与尾核长度；若周期等于物理 stride，转向搬运和布局。

## 动态分支边界审计

当两个相邻 shape 的误差、tiling key 或 kernel 符号突然变化时，先从 Host 条件或配置中找出真实边界 `B`，再测 `B-1`、`B`、`B+1`。例如真实边界为 512 时使用 511/512/513；这三个数只是相邻边界模板，不是通用 tile 常量。

每个 shape 用同一张表记录：

| 项目 | 必须保存的观察 |
|---|---|
| Host 判定 | 实际参与判断的维度、原始条件文本及 `<`、`<=`、`==` 的命中结果 |
| 分派身份 | tiling key、kernel 符号、编译分支和产物摘要 |
| 工作划分 | blockDim、每核区间、tileLen、tileNum、tail 长度和有效元素集合 |
| Kernel 实收 | 入口实际收到的 shape、属性和 tiling 字段，而不只看 Host 预期值 |
| 数值边界 | 最后正常 checkpoint、最早异常 checkpoint、错误坐标所属 tile/core |

若 511 与 512 选择不同分支，先分别验证两条分支对自身合法输入的语义；若 Host 在 `B` 选择分支 A，Kernel 却按分支 B 的字段解释，或等号边界存在遗漏/重叠，支持动态分派合同候选。若三个 shape 使用同一分支但只有尾块失配，则降低分派候选，转查有效长度、mask 和尾状态。若允许做诊断性强制分支，执行前写出每条分支在三个 shape 上的预测，并证明实际产物采用了强制结果；强制通过只说明结果依赖该路径，不能证明 Host 判定就是唯一根因，也不能直接作为正式修复。

换 shape 会同时改变数值集合、核数、tile 数和 tail，因此“换 shape 后恢复”不是分派证据。只有相邻输入、实际分支身份、入口字段和首偏离边界共同符合事前预测，才能支持边界条件候选。具体长度阈值、tiling key 名称和分派规则须从当前工程确认。

### 零工作量 producer 与旧值候选

尾块或条件分支后消费者读到上一轮数据时，先单列“本轮 producer 根本没有更新该 region”的候选，再检查地址、generation 和同步。记录实际采用的重载、字段值、目标 region 的写前/写后观察，以及随后 Wait/读取所在的可达路径；没有新写入时，后续读到的可能是旧值或未定义残留，不能直接归因为 Wait 过早。

固定 CANN 9.1 来源给出三类有边界的例子：Atlas A2/A3 的高维 Memory DataCopy 中 `blockCount` 或 `blockLen` 为零是 NOP；Ascend950PR/DT 的 MX `LoadData` 重载（来源页 `Load2DMX.md`）规定量化系数参数 `LoadData2DMxParams` 中 `xStep` 或 `yStep` 为零是 NOP；基础 Memory `TransDataTo5HD` 的 `TransDataTo5HDParams.repeatTimes=0` 不执行转换且不写目的操作数，适用 950PR/DT、Atlas A3、Atlas A2、Atlas 200I/500 A2、Atlas 推理系列 AI Core 和 Atlas 训练系列产品，不适用于 Atlas 推理系列 Vector Core。它们分别适用于各自的 API、产品和通路，不能由字段同名推广到其它 DataCopy、LoadData、转换重载或 profile。

区分实验可在冻结地址、输入、同步对象和其它 tiling 字段后，对零值与最小合法正值各自运行一次，先在 producer 边界确认目标 region 是否被写入。两组的工作量和数学输出合同不同，因而不以最终数值相等作为通过条件；只比较每组在其自身有效区和 oracle 下的结果。若正值路径更新了正确 region 而零值路径没有更新，支持零工作量候选；若两者均更新却消费者仍错，再回查映射、生命周期和 happens-before。

## 多层分段状态：row、head、N-segment 与 K-pass

当首行/首 head/首 N 段/首 K-pass 正确，后续层级才异常时，不要把所有循环都压成一个 `tileId`。为每个 checkpoint 保存完整 epoch：

```text
(core, row, head, nSegment, kPass, tile, stage)
```

并分别记录 data 与 scale/parameter 的 GM offset、L1 head offset、L0 起址、LoadData 字段、物理 layout/stride、valid M/N/K，以及该 pass 是初始化还是累加。第一 N 段正常而后续段异常时，优先检查 data offset 与 scale offset 是否都随 `nSegment` 更新；第一 K-pass 正常而后续 pass 异常时，检查 K 起址、长度单位、scale 起址、累加初始化状态和上一 pass 的最后消费者。数据 offset 更新正确并不能证明 scale offset 也正确，两类载体可能有不同 dtype、分形和物理 footprint。

以单 K-pass、单 N 段或单 head 强制运行只能作区分实验。它会改变 L0/L1 占用、MMAD 累加次数、舍入结合顺序和流水回收，因此“单 pass 通过”只支持分段状态/地址候选。只有错误边界随事前预测的 `nSegment`/`kPass` 移动，并在修正一条数据边后其最早分叉同步移动，才可确认相应分段合同。

`K_BASE` 是否必须整除某个单位、`yStep` 的单位、N 段 scale 起址和 head offset 公式都属于精确 LoadData 产品/重载/layout 的强事实；应查询当前 API 页。当前 profile 不支持的 MX 接口公式不可借来解释 910B1，K_BASE、MX 和 head-offset 的具体公式须与当前重载及布局一致。拿不到物理 layout、目标产品或实际采用的生成路径时保持 UNKNOWN。

## UB 与实验混杂

UB 预算必须按物理字节、对齐开销、buffer 个数和生命周期计算。扩大 dtype、改变 double buffer 或调整 tile 会同时改变容量和调度，只能作为有预测的实验。硬件容量、接口返回值和最小对齐属于平台或 API 强事实，不写成跨版本常量。

预算用符号化账本表达：每个存活槽位的 `aligned(elements × sizeof(dtype)) × instances`，再按同一时刻的生命周期求和并加入合同要求的保留空间。不同 TBuf 名称不保证物理独立，ReinterpretCast 或 offset slice 也不产生新容量；反之，double buffer 的实例是否同时存活必须按真实队列/流水合同核对。

升精度实验会同时扩大 UB 占用并可能改变 tileLen、tileNum、分支 key、归约顺序和流水调度。若结果变化，先证明哪一项实际被操纵；不能把“fp32 通过”直接解释为 fp16 算术精度不足。

### 升精度后的 UB 峰值生命周期账本

当候选修复把某段中间计算升精度、增加 Cast 临时量，或升精度后开始编译失败、全零、局部损坏、tileLen/tiling key 改变时，查询本节。不要只把所有声明字节相加，也不要只把改 dtype 的一个 buffer 乘二；应按当前真实分支建立峰值账本：

| 字段 | 核对内容 |
|---|---|
| 物理存储 | TBuf/TQue/静态区、alias group、真实分配点与实例数 |
| 字节 | 元素数、dtype 字节数、当前 API 要求的对齐后大小 |
| 生命周期 | 首次可能写入、最后一次消费、释放或允许复用的事件 |
| 并存阶段 | CopyIn、Compute、Cast、CopyOut 及 double-buffer 前后轮是否同时存活 |
| 运行身份 | shape、tiling key、tileLen、tileNum、blockDim 与实际产物 |

先按当前版本的真实分配器语义计算**保留占用**：每个 `InitBuffer`/静态分配及其实例的对齐后字节都要累计；即使两个张量的数据生命周期不重叠，只要没有地址、内存池或释放语义证明它们复用了同一物理区，就不能从容量账本中折叠。随后再按生命周期计算**并存数据峰值**，用于判断显式 scratch 复用、alias 和提前覆盖是否安全。共享基址的 alias 在两张账本中都只计一次物理区，但只要任一 alias 尚需读取，该区就不能被下一用途覆盖。新旧 tile 的队列实例、升精度临时量和低精度 CopyOut 缓冲在转换阶段可能同时存活，因此并存峰值也不一定等于稳态单 tile 用量。

区分实验应先固定 shape 和分派，分别保存升精度前后的账本与实际 InitBuffer/静态分配证据，再只调整一个生命周期或 tile 参数并预告峰值如何变化。若结果改善同时伴随 tileLen、分支或核数改变，只能说明资源/调度相关，不能确认哪块 buffer 超限。硬件 UB 容量、对齐单位、队列实例语义或某 API 是否隐式分配属于版本/平台强事实；当前权威节点缺失时保持 `UNKNOWN`，不用历史案例常量补齐。

反例：两个大型临时量生命周期不重叠，但各自由不同静态 `InitBuffer` 保留地址；只算活跃数据会误判容量足够。只有分配器合同或实际基址证明它们复用同一物理区时，才可按生命周期折叠容量。反过来，只按“fp16 改 fp32 所以该槽翻倍”计算，又会漏掉 Cast 期间新旧表示与 double-buffer 下一轮输入的并存峰值。UB 容量、安全余量、buffer 数量或对齐常量不作为跨版本事实，必须查询当前版本 官方 API 文档及本地平台说明。

## 平台容量与库预留

存储层次、核类型和跨产品容量差异调用 CANNBOT `npu-arch` 技能查询；典型硬件参数不能代替本次运行端的可用容量。这里继续检查当前算子的分配、预留和生命周期，具体 API 的单位与约束仍调用 CANNBOT `ascendc-docs-search` 技能查询。

换设备或增加高阶 API 后开始 tile 超预算时，分别记录平台总容量、库内部预留、当前算子的物理分配和活跃数据峰值。Host 与 Kernel 查询值不同时，先比较查询入口、单位、执行形态和预留阶段；多次配置还要确认最终采用的状态。容量、核数与预留入口查[Host 运行时](tiling-tail-state.md#查询容量时区分阶段与用途)，不沿用旧设备常量。

CalcTschNumBlocks 等入口的 AIC/AIV 配置与 blockDim 会改变每核工作量和尾块。检查 Host 计算值是否原样进入 Kernel，并与[实际参与核](multicore-ownership.md#启动核参与核与空核)对照。系统/用户 workspace 的容量与起点另见[workspace 分区](buffer-lifetime-alias.md#系统与用户-workspace-的分区)。

## 收口与回归

正式修复需重新评估 dtype 改变后的 UB 字节预算，并回归边界两侧，而不是把 tile 常量调到当前样例通过。

回归至少覆盖单 tile、两 tile、整除、余 1、最大尾块、动态区间两侧、首行/后续行和不同核所有权。如果修复是参数重载，验证每轮 producer；如果修复是分支边界，验证 Host 与 Kernel 使用同一合同。

当现象落在最后一个有效块或前一 generation 时，可按需打开 [尾块有效区与旧状态：先证明本轮写入](../cases/tail-valid-region-state.md)；它把本页的有效区、零工作量和复用候选组织成可否证对照，不新增 API 事实。

### 查询容量时区分阶段与用途

GetCoreMemSize 的平台容量、ReserveLocalMemory 的预留与 GetRuntimeUBSize 的运行时可用量服务于不同阶段，不能要求它们无条件相等。记录查询位置、单位、执行形态和预留状态，再判断当前物理分配与活跃数据是否超限。具体入口查[平台容量](https://gitcode.com/cann/asc-devkit/blob/e5451c7feba11f9e259f4fbc19c4bf82f8c0496c/docs/api/Utils-API/平台信息获取/PlatformAscendC/GetCoreMemSize.md)和[运行时 UB](https://gitcode.com/cann/asc-devkit/blob/e5451c7feba11f9e259f4fbc19c4bf82f8c0496c/docs/api/SIMD-API/基础API/工具接口/系统资源与变量/GetRuntimeUBSize.md)，当前版本差异调用 CANNBOT `ascendc-docs-search` 技能确认。

系统 workspace 与用户 workspace 分别核对容量和起点；总量正确但用户指针偏移不对，仍可能覆盖系统区。资源账本与对照方法见[平台容量与库预留](tiling-tail-state.md#平台容量与库预留)和[workspace 分区](buffer-lifetime-alias.md#系统与用户-workspace-的分区)。核数、启动参数和编译形态属于实际执行身份，应从运行端取得，不能用 Agent 本机代替。
