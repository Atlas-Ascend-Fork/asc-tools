# 搬运、对齐与有效区

当错误首次出现在 GM 与片上存储之间、行尾或非对齐 shape 时，先冻结五个互不等价的量：逻辑元素数、物理分配字节数、单次搬运长度、计算 mask 覆盖长度和最终有效输出区。任何一个量经过向上对齐，都不能自动替代另外四个量。

## 建立数据边合同

对每条可疑边记录：producer、源物理位置和 offset、API 精确重载、每个参数的单位、目的位置和 offset、consumer、有效元素区间。将 stride 写成地址公式，并分别计算第 0、1、末块的实际地址；不要只比较参数名。

再为同一条边建立三单位账本：逻辑元素 offset/count；对于字节寻址且没有打包/压缩/格式转换的表示，再用 `sizeof(dtype)` 换算字节地址/count；接口字段名、实参值及该精确重载声明的单位。bit-packed 或格式转换路径按实际 packing、data block 与 layout 合同换算，缺少定义时保持 UNKNOWN。物理 block count/形状、有效区和 padding 另列，不能用块数替换元素数或字节数。接口单位和支持能力属于版本强事实；应路由到当前版本的数据搬运/布局 API 页，不从旧案例或参数名称猜测。

若尾块只留下前一 generation 的图样，按需查看 [尾块有效区与旧状态：先证明本轮写入](../cases/tail-valid-region-state.md)，先验证本轮 producer 是否覆盖消费者的有效 region，再判断搬运、mask 或同步。

## 可证伪实验

1. 使用共享前缀相同的对齐与非对齐 shape，只改变有效长度，并确认 tiling key、核数和分支可比。
2. 在搬运前、搬运后、首次消费后三处保存同一逻辑元素及其物理 offset。
3. 将错误索引按行、data block、repeat 和核区间重排，检查周期是否与某个 stride 或 padding 一致。
4. 若改换重载，先按目标版本重新计算参数单位和目标缓冲区容量，再比较结果。

## 布局、Stride 与 Padding 分流

把逻辑 layout 和物理搬运分开：ND/NZ/分块布局决定坐标如何落到物理位置，搬运 API 决定这些物理区间如何复制。对 reshape/transpose 后的 Tensor 先判断是否连续，再把元素 stride、字节 stride 和接口参数单位分别写入表中。错误周期等于一行或一个通道块时，用可逆坐标编码区分轴映射错误与纯搬运长度错误。

对 transpose/reshape view，先由轴映射推导每一轴的元素 stride，再换算字节地址；不能因逻辑 shape 相同就假设物理连续。构造一组配对诊断：原 view 与按同一逻辑值显式物化的连续 staging Tensor，分别在消费者前后比较值和地址。只有 staging 通过时，才提高非连续访问、stride 或搬运参数候选。staging 同时可能改变对齐、buffer 占用、tiling 和同步路径，必须记录这些差异，不能把该 control 直接升级为正式修复。

### 多行批量搬运与逐行搬运对照

当 `blockCount>1` 的批量搬运只有第二行以后错误，而逐行调用看似正常时，查询本节。先为逻辑行 `r` 写出同一对照目标：

```text
src_head(r), valid_bytes(r), src_physical_pitch
dst_head(r), dst_physical_pitch, valid_elements(r), padding/dummy 区
```

批量路径用一次调用的 `blockLen + src/dst stride` 推导这些地址；逐行路径用每次 Tensor offset 推导。只有两条路径的第 0、1、末行逻辑坐标、物理起址、valid 区和目标 buffer 容量完全相同，才是地址语义上的 control。批量搬运减少调用次数是性能候选，不是正确性证明；逐行搬运还会改变 DMA 提交次数、EnQue/DeQue 节奏和 Double Buffer 重叠，因而“逐行通过”可能同时掩盖 stride、padding 或流水回收问题。

先做三个有预测的实验：把 `blockCount` 从多行缩到 1 但保持相同行 pitch；保留批量调用而只让 stride 表达连续行；给每行首尾写入可逆 `(row,col)` 编码并比较第一个错行。若错误边界随 `blockCount`/stride 预测移动，支持批量地址合同候选；若只随 Queue/slot 节奏变化，转查生命周期与同步。缺少精确重载单位、UB 物理行宽或每行有效长度时结论为 UNKNOWN。

### Padding 位置、物理 pitch 与归约集合

在 CANN 9.1 Ascend950PR/DT 的 GM→UB `DataCopyPad` 入口中，Normal 会让每个非对齐数据块补齐，而 Compact 将数据块合并后只在整块末尾补齐；Compact 下 `dstStride`、`leftPadding` 和 `rightPadding` 不生效。这是该产品、方向和重载的布局事实，不能推广到 A2/A3 或其它搬运通路。

因此多行数据进入消费者时必须同时写出 `row_head(r)` 的**物理 pitch**和本行的**逻辑有效元素数**。CANN 9.1 Memory `ReduceSum` 的 count 入口需按产品解释：在支持 `isSetMask` 的产品上，默认 true 时由 `count` 指定参与元素数，false 时由调用前设置的 mask 决定；在本节的 Ascend950PR/DT 上，`isSetMask` 不生效，仍按 `count` 归约。其他产品的参数支持与其他重载分别查对应 API。count 不替代行首寻址，也不自动把 Normal 的逐行 padding 或 Compact 的末尾 padding 变成有效输入。

比较 Normal 与 Compact 的实验先按各自合法布局解码同一 logical region，再比较逻辑元素和相应的归约结果；raw UB 字节或物理 pitch 不同本身不是失败。需要观察 padding 时，使用当前 API 已定义的 `paddingValue` 或明确配置的 SetPadValue；未由当前条件定义的 dummy 不写预期值。随后只改变 mode，保持逻辑输入和消费者合同不变：若错误随解码后的 row head 或有效 count 预测移动，支持布局/集合候选；若逻辑 region 在搬运后已正确，转查后续计算、别名或同步。

### 共享 L1/L0 的多阶段边合同

GM→L1、L1→L0、Cube/Vector 消费和写回之间共享同一片 L1/L0 时，每阶段单独“shape 正确”仍不足。建立阶段表，逐边记录逻辑 shape、dtype、物理 format、C0/分形、起始 offset、head-to-head stride、有效尾块、producer/consumer 和存活区间。下一阶段的源合同必须与上一阶段实际写出的物理布局一致；不能让前一阶段按 ND/NZ 的一种 stride 写入，后一阶段却用另一种 shape 或转置标志解释同一地址。

对共享 buffer 同时检查两条轴：空间上，第 0、1、末分形地址是否按同一 layout 公式闭合；时间上，下一 stage 或下一 head/tile 是否在最后消费者之前覆盖该区。独立 staging 通过时，只能提高 layout、stride 或 alias 候选，因为 staging 同时改变地址、容量与调度。LoadData 的 C0、字段单位和产品支持必须转查版本 API 页；如果只能取得逻辑 shape 而拿不到物理 format/stride，保持 UNKNOWN。

Padding 只属于物理存储时，后续计算 mask、Reduce count 和最终比较必须排除无效 lane。不要看到尾部脏值就直接扩大 padding；先在搬入后、计算前、搬出前比较同一有效元素。GlobalTensor 标量写、LocalTensor 标量写和 DMA 搬运的可见性路径不同，若地址和值正确但消费者看到旧值，转查同步或 Cache 可见性，而不是继续调整 `blockLen`。

DataCopy/DataCopyPad 为 DMA 对齐形成的 padding/dummy 与 Conv/Load3D 的逻辑空间 padding 必须分账：前者描述物理传输与无效 lane，后者参与窗口坐标和数学边界。DMA padding 值即使恰为 0，也不能据此改变卷积 padList；Conv padValue 即使已设置，也不能证明 GM↔UB 尾块 dummy 已初始化。

## 筛选后的有效区与状态

GatherMask 后的物理 Tensor 容量不等于本轮筛选结果长度。记录返回的有效数（如当前接口的 rsvdCnt），只把有效元素交给搬出、统计与下一次计算；尾部随机值只有被消费时才影响结果。自定义掩码编码不匹配 dtype、多 repeat 读取范围重叠，也会表现为选出的元素或数量不对；调用 CANNBOT `ascendc-docs-search` 技能核对掩码规则，再展开实际读取位置。

若第一轮正确、后续轮次异常，同时检查掩码模式与输入复用：当前 GatherMask 是否改变了后续计算依赖的模式，DropOut 是否修改了随后还要使用的 mask。分别保存调用前后状态与掩码内容，在确需的段落设置/恢复模式，或为可变输入保留正确副本。DropOut 的保留位、缩放位置与 dtype 仍按原算法核对。

Gather/Scatter 的 offset 先换算为真实字节地址，再检查边界与目标位置是否重复。重复 Scatter 位置涉及写入所有权，不能只靠调整偏移单位修复，另见[索引与地址归属](index-address-ownership.md)。逐行与多行搬运的容量、单位及对齐问题继续使用上方的成对地址实验。

## 跨架构迁移时重画数据通路

存储和计算单元的角色可以复用；**具体连接通路、编程能力与资源规模**需要保留架构条件。下面是 2201 → 3510 迁移中对读代码有直接影响的差异，依据 CANN 9.1.0 的迁移指南，属于选摘而非完整支持矩阵。[2201 到 3510 架构变更][movement-source-7]

| 项目 | 2201 | 3510 |
| --- | --- | --- |
| Vector 计算方式 | Membase | Regbase，支持 SIMD Reg、SIMT 及混合编程 |
| GM → L0A/L0B | 存在直接通路 | 该直接通路移除，需经 L1 中转 |
| UB → L1 | 没有该直接通路，通常经 GM 中转 | 新增直接通路 |
| L0C → UB | 没有该直接通路，通常经 GM 中转 | 新增单向直接通路 |
| L1 → GM | 存在直接通路 | 该直接通路移除；调试搬出同样受影响 |

迁移时先把数据流重新画通，再核对搬运、转换与同步的 API 条件。上述表格只比较 2201 和 3510，不用于判断其他架构；某条通路存在也不代表任意 dtype、layout 或搬运接口均可用。

[movement-source-7]: https://gitcode.com/cann/asc-devkit/blob/e5451c7feba11f9e259f4fbc19c4bf82f8c0496c/docs/guide/跨代迁移兼容性指南/3510架构迁移指导/2201到3510架构变更.md

## 结论边界

- 非对齐 shape 失败只说明路径依赖 shape；它不证明接口不支持非对齐。
- 尾部出现旧值可能来自 CopyOut 长度、计算 mask、目标容量或同步，不能只归因于 padding。
- 结合[msSanitizer](https://gitcode.com/Ascend/mssanitizer)的访存告警核对实际访问，仍需证明逻辑 offset 与有效区是否正确。
- 搬运后值正确而首次消费后错误时，应降低搬运候选，转向计算 mask、别名或同步消费边。
- 布局转换前后逻辑坐标一致而物理地址不同是正常现象；只有实际坐标映射与合同不符才构成缺陷。

确认根因前，不把“扩大 buffer”“全部按 32B 处理”作为通用修复；这类改动可能掩盖越界并把无效 padding 纳入计算或验收。

## 事实边界

DataCopy/LoadData 的地址单位、padding 行为、C0 与产品支持必须以当前版本 API 文档为准；先确认这些条件，再使用多行批量/逐行对照与逐阶段布局账本。
