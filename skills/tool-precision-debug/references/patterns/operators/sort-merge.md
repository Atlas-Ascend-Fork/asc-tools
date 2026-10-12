# 排序与归并算子族

## 数学与 reference

Sort 生成有序 score/index 记录，MrgSort 合并最多若干有序队列；稳定性、有效长度和记录布局决定 oracle。

## I/O 与实际链路

- 冻结输入/输出 shape、dtype、layout、有效区、padding 和 alias。
- 从 host tiling、GM 搬入、局部计算、同步到 GM 搬出逐节点记录实际计算链，不以 API 名替代。
- tiling 分支、多核分区、尾块与特殊值必须进入风险清单。

## 敏感 checkpoint

按疑点从输入解码、局部排序、归并、记录搬运和最终有效输出中选择少量相关观察，说明观察面与参考依据。没有完整局部 oracle 时，可先检查排序、配对或数量守恒等适用性质，结论限于所检查的性质。选点和缺口处理见[观察点设计](../../diagnosis/checkpoint-design.md)；已有可比数值误差需要解释传播时，再用[逐层分析](../../diagnosis/progressive-error-analysis.md)。

## value-index 指纹与并列合同

排序记录不能只验 score。给每个输入位置配置唯一、可逆的 index 指纹，逐条核对输出 `(value,index)` 仍来自同一输入记录；“value 已有序”不证明 index 配对正确。重复 value、`+0/-0`、NaN、相同 score 的稳定性以及 TopK 截断边界必须由当前任务合同明确；未规定稳定顺序时，不能把任意一种 tie 顺序写进 golden，也不能用集合相等掩盖明确要求的稳定性。

至少加入三组 control：全部 score 唯一，验证纯排序与配对；部分 score 相同，验证 tie/stability；截断点两侧相同或相邻，验证 TopK 边界。padding 或无效 proposal 使用与有效记录可区分的 poison，但只有物理缓冲区已分配、合同允许读写且 poison 本身是接口语义允许的值时才设置；否则保持 UNKNOWN，并通过有效长度成对输入观察。

### Record/Proposal 有效区账本

- **何时查**：排序值正确但 index 丢失、尾部出现旧记录、日志把 `record` 与 `proposal` 混称，或 CopyOut 长度和 TopK 长度不一致时查询。
- **查什么**：对每个阶段分开记录 `capacity`、`produced`、`logical_valid`、`consumed`、`emitted`、`compared`，并记录物理记录跨度和字段解码依据。`capacity` 不能替代有效记录数；旧材料叫 proposal 只作为检索标签，当前记录是否为 score/index、Region Proposal 或其它布局必须由真实重载与版本 API 裁决。
- **怎么证伪**：保持容量不变，只改变有效长度，并让有效记录带唯一指纹；仅当无效区已分配且允许诊断写入时才改变其内容，否则只做有效长度对照。若最终输出严格跟随有效长度、允许的无效区变化不影响有效结果，且每阶段计数守恒，就降低有效区泄漏候选。
- **反串线**：无效区出现任意值不等于越界，只有它进入 `consumed/emitted/compared` 才形成证据；记录字节数、字段偏移、Extract/ProposalExtract 含义和产品支持均调用 CANNBOT `ascendc-docs-search` 技能核对当前原文，未知即 `UNKNOWN`。

## 多路归并守恒账本

每轮归并为每一路记录 `offset_before`、`remaining_before`、`consumed`、`remaining_after`，并记录本轮 `emitted` 和输出有效区。必须满足：每路 `remaining_after = remaining_before - consumed`，所有路的消耗总数与本轮真实输出数按接口语义对账，offset 只前进已消费记录的物理距离。一路耗尽后仍出现重复记录，先查有效路集合、紧凑返回计数与 offset 更新；总条数减少则查漏消费、过早退出或 TopK 截断合同，不先归因数值排序。

守恒通过仍不证明顺序正确；顺序通过也不证明 value-index 配对和截断正确。分别使用每路不同量级的 value/index 指纹、单路耗尽、两路同时耗尽、只剩一路和恰好跨 TopK 边界的输入。`validBit`、返回数组含义、proposal 布局、偏移单位及产品支持调用 CANNBOT `ascendc-docs-search` 技能从当前版本的排序与归并原文确认。

### 归并进度量与截断边界

- **何时查**：归并卡住、重复上一批、一路耗尽后越界、输出超过/少于 TopK，或下一 group 才异常时查询。
- **查什么**：把一轮拆成“确定有效路与输入有效长度 → 搬入 → 合并 → 读取逐路消费量 → 更新剩余量与 offset → 搬出 → 必要时清理 group 状态”。选择一个严格进度量，例如所有有效路 `remaining` 之和；每轮还记录停止谓词的轮前值和轮后值。
- **怎么证伪**：覆盖首轮、一路提前耗尽、只剩一路、尾 group 和恰好到截断点；若每个非终止轮次进度量严格下降、输出 offset 只按真实 emitted 前进、重新初始化后不受上一 group 影响，就降低状态机候选。
- **反串线**：循环终止只证明有进度，不证明队列输入已排序、tie 合同正确或截断记录正确；反之，最终 score 有序也不能证明没有重复/漏项。逐路返回计数是否紧凑、有效路编码与 offset 单位属于 exact overload 事实，只从版本页取值。

## 接口组合中的排序异常

- 输出每小块有序而整体无序时，核对是否只做了局部排序、是否启用所需的全排序模式，以及后续归并是否真的消费了全部有效路。
- sortedNum/GetMrgSortResult 用于推进循环前，先确认当前模式下计数有效。有效路描述、实际队列长度和返回计数布局一起核对，不能假设无效槽位自动清零或把路数直接当 validBit。
- proposal 地址差一个固定倍数时，检查字节、元素、记录跨度是否重复换算。GetSortLen/GetSortOffset 的文档与实现存在单位分歧时，按[排序单位说明](sort-merge.md#排序偏移的单位分歧)做小规模地址对照，不统一套倍数。
- half/BF16 score 已经相等时，扩大为 float 不能恢复窄化前的区别。需要更高分辨率，应在首次窄化前保留；输入只剩低精度值时按其真实值和任务的并列规则排序。

不同重载的 repeat、零次数、有效路编码与补齐要求调用 CANNBOT `ascendc-docs-search` 技能逐项核对，本页继续检查有序范围、进度和数量是否正确。

## 排序偏移的单位分歧

CANN 9.1 的 [GetSortOffset 文档](https://gitcode.com/cann/asc-devkit/blob/e5451c7feba11f9e259f4fbc19c4bf82f8c0496c/docs/api/SIMD-API/高阶API/排序操作/GetSortOffset.md)与[配套实现](https://gitcode.com/cann/asc-devkit/blob/e5451c7feba11f9e259f4fbc19c4bf82f8c0496c/impl/basic_api/kernel_operator_proposal_intf_impl.h)对单位的表述不一致。两边同时保留，结合当前版本与重载做最小布局对照：使用唯一 value/index 指纹，检查相邻记录真实地址差、有效记录数与分配跨度，区分字节、元素和记录单位。尚未确认时不套用任何一方的倍数，也不把小样本通过推广到其他重载。

## 版本路由与边界

排序与归并的接口资料调用 CANNBOT `ascendc-docs-search` 技能获取。本页为诊断模型，不代表任一具体实现或 NPU case 已验证。
