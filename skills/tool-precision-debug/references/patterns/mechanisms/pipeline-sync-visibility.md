# 流水同步与数据可见性

同步问题必须写成 producer 流水、consumer 流水、共享资源和缺失依赖四元组。错误位置随运行变化、只在多核出现或插入 barrier 后改善，都只是候选信号；全局停顿可能同时掩盖覆盖、生命周期和缓存问题。

先把将要解释的观察放回竞争候选中：

| 当前观察 | 可以支持 | 下一步仍需区分 |
|---|---|---|
| 全流水屏障后恢复 | 改变执行时序或重叠能影响结果 | 提前发布、复用覆盖、generation/地址错配、核内数据就绪等候选 |
| 缩小到单个流水屏障后再次失败 | 该细化处置未保留原探针的效果 | 原先的具体依赖解释是否成立，细化是否实际生效，是否还有其他被全局停顿掩盖的边 |
| 两个子块使用同一个 event 数字 | 存在需要审阅的资源身份线索 | 封装最终调用的 API、执行侧、依赖类型和资源作用域；不能直接假定子块共享事件 |

对当前 profile 的 subblock、Set/Wait 或 PipeBarrier 语义有疑问时，调用 CANNBOT `ascendc-docs-search` 技能取得对应接口正文，并展开工程的同步封装；源码中的 wrapper 名称不一定是公开 API。不要在尚未确认作用域时先给所有 event 重新编号。

按三层审计依赖：编译器可能提供的自动同步、队列/资源框架表达的 producer-consumer 协议、开发者显式维护的 event/barrier/cache 可见性。任一层是否覆盖当前边都属于具体版本和编程范式事实，不能从另一工程或同名 API 外推。

## 区分三类边界

- 核内不同流水依赖：核对精确编程范式是否由编译器、TQue 或开发者负责插入同步。
- 同一流水地址重叠：核对目标版本的 PipeBarrier 约束，不把跨流水 Set/Wait 与同流水 barrier 混用。
- Scalar 与 DMA/其他核的 GM 可见性：单独检查 Data Cache、Cache Line 所有权和核间同步。

### 先按依赖边选 API 族，再查版本事实

| 实际依赖边 | 优先查询的版本 API 族 | 不能替代它的相邻机制 |
|---|---|---|
| 同一核、不同流水的 producer→consumer | `SetFlag/WaitFlag` | 同流水 `PipeBarrier`、跨核 flag |
| 同一流水内部的地址/指令依赖 | `PipeBarrier` | 跨流水 event、全核 barrier |
| 所有实际参与核必须到达同一阶段 | `SyncAll` soft/hard | Cache flush、Atomic、单核 Set/Wait |
| 分离模式下跨核计数通知 | CrossCore Set/Wait flag | 同核 event ID、普通流水 barrier |
| 多个 contributor 向共享 GM 累加 | 先区分 DMA Atomic 状态与直接标量 Atomic | 核间 barrier 不定义数值累加顺序 |

该表只做诊断路由，数值、产品支持、workspace、mode、flag/event 范围和 dtype 均调用 CANNBOT `ascendc-docs-search` 技能查目标环境的原文。对每次实验记录 producer/consumer、地址、流水或核、event/flag generation、参与核集合，以及 Atomic 开启和关闭点。编译器可能自动插入某类同步时，仍须从生成物或同步日志证明它覆盖的是这条真实数据边。

还要区分 LocalTensor 上的 Scalar 写后由 DMA 消费，与 GlobalTensor 标量写后要求 GM 可见；两者的地址空间和依赖边不同。若现象是“上一轮旧数据”，检查多 tile 复用边是否形成循环闭环；若现象只在多核，先证明跨核地址或依赖，再使用 [多核分区、所有权与核间协同](multicore-ownership.md)。

`GlobalTensor::SetValue` 后输出全零或其他消费者仍看到旧值时，先记录写入发生在 Scalar DCache、后续消费者通过 Scalar、DMA 还是其他核读取，以及需要可见的最早时点。分别核对受影响的 Cache Line、多核是否共享同一行和当前产品的 Cache 一致性 API 合同；不能把“索引 0 正确”当成 GM 已可见，也不能默认改成 `LocalTensor::SetValue + DataCopy` 就是唯一正确修复。精确刷新原型、范围和产品支持必须从当前 CANN API 文档读取。

## 从执行单元检查依赖

不清楚目标架构的执行单元、指令队列、数据通路或 VF 形态时，先调用 CANNBOT `npu-arch` 技能建立对应关系；本页据此检查实际 producer-consumer 依赖，具体同步接口继续调用 CANNBOT `ascendc-docs-search` 技能查询。

Scalar 发射的计算和搬运指令进入不同队列，各执行单元可以异步工作。**代码先写搬入、后写计算，并不单独证明计算开始前搬入已完成。** 即使在同一流水内，指令按顺序进入，也不意味着前一条的全部读写已经结束。[硬件基本架构][sync-source-9]、[同步控制简介][sync-source-6]

由这个执行模型，可以把缓冲区上的依赖分成三种来理解：

| 依赖 | 必须满足的先后关系 | 典型场景 |
| --- | --- | --- |
| 写后读 | 生产者写完，消费者再读取 | GM → UB 搬入完成后，Vector 读取这一块 |
| 读后写 | 消费者读完，下一轮再覆盖 | UB → GM 搬出尚在读取时，不能提前覆盖同一输出缓冲区 |
| 写后写 | 需要保留的两次写入按预期顺序完成 | 不同阶段写同一地址，最终值需要明确由哪次写入产生 |

双缓冲通过交替使用不同缓冲区为搬运与计算重叠提供空间，但每个缓冲区被再次使用时，仍要满足上述依赖。核对依赖时，应同时考虑框架、编译器已经建立的同步，以及直接指针、复用或手工搬运是否超出其覆盖范围。

同步还要区分作用范围：

| 范围 | 需要理解的保证 |
| --- | --- |
| 核内单流水 | `PipeBarrier` 约束相应流水中前后指令的读写完成顺序；具体可用形式看架构 |
| 核内不同流水 | 队列同步或匹配的 SetFlag/WaitFlag 表达生产者完成、消费者再继续；要识别具体流水方向 |
| Reg VF 内 | 寄存器依赖由硬件保证；不同寄存器访问同一 UB 区域产生的读写依赖仍可能需要显式同步 |
| SIMT Block 内 | 同步屏障用于线程间阶段协作；内存栅栏约束当前线程的访存顺序和可见性，不等待其他线程到齐 |
| 不同 Block 或不同核 | 需要该范围的通信与同步设计，核内流水同步和 Block 内屏障不能扩展为全局同步 |

前三行依据[核内同步说明][sync-source-6]和[Reg 矢量计算编程][sync-source-10]；SIMT 的屏障、栅栏与 Block 边界见[SIMT 同步机制][sync-source-14]。具体核内、SIMT 或跨核接口调用 CANNBOT `ascendc-docs-search` 技能核对。同步只解决相应依赖；索引、布局、类型或计算公式错误仍需分别核对。

[sync-source-9]: https://gitcode.com/cann/asc-devkit/blob/e5451c7feba11f9e259f4fbc19c4bf82f8c0496c/docs/guide/编程指南/高级编程/硬件实现/基本架构.md
[sync-source-6]: https://gitcode.com/cann/asc-devkit/blob/c8c1b9df6bb9cd61c09751c7ef414b89e06af274/docs/api/context/同步控制简介.md
[sync-source-10]: https://gitcode.com/cann/asc-devkit/blob/e5451c7feba11f9e259f4fbc19c4bf82f8c0496c/docs/guide/编程指南/编程模型/AI-Core-SIMD编程/基于指针的C语言编程/Reg矢量计算编程.md
[sync-source-14]: https://gitcode.com/cann/asc-devkit/blob/e5451c7feba11f9e259f4fbc19c4bf82f8c0496c/docs/guide/编程指南/编程模型/AI-Core-SIMT编程/同步机制.md

## 因果实验

固定同一输入重复运行，记录错误索引集合和每核区间。只在推定边上加入一种同步，事先写出改善应发生在哪个张量、哪个阶段；同时查看编译器同步日志或生成物，确认干预确实进入目标产物。若删除可疑并行重叠后仍稳定失败，则降低竞态候选。

对 Global 标量写可做三组单变量对照：保持地址不变只改变消费者通路，保持通路不变只改变可见性处理，保持每核数据不变只隔离 Cache Line 所有权。只有观察变化符合事前预测，才能区分 Cache 可见性、核间覆盖和批量搬运路径问题；诊断用全 Cache 操作不能直接成为正式修复。

对 event 类候选记录每次获取、Set、Wait、释放和循环复用点，核对方向与生命周期，而不是只搜索相同数字。单 tile/多 tile、单次/重复运行、一个 producer/多个 consumer 三组对照可区分缺少依赖、事件复用和资源别名。[msSanitizer](https://gitcode.com/Ascend/mssanitizer)报告或编译日志提供原始观察，不把错误码直接映射成根因。

event ledger 至少包含资源身份、依赖方向、申请/获取点、Set、Wait、释放和下一次复用；同一个数字出现在不同依赖类型中不自动代表同一资源，也不能用一次成功运行证明生命周期始终合法。

### 把嵌套循环展开后再放置 Set/Wait

同一对 Set/Wait 在源码中各出现一次，不代表实际执行次数配对。先展开一个真实的首轮、中间轮、尾轮，分别数清每个资源和 generation 的发布、等待与复用；空循环、条件跳过和最终收尾也算执行路径。下面是诊断模型，具体事件行为仍以当前版本 API 为准：

| 实际发布粒度 | 容易漏掉的等待路径 | 区分检查 |
|---|---|---|
| producer 完成一整轮的多个 region 后 Set 一次 | consumer 对每个 region 都 Wait 一次 | 后续 Wait 是否有各自对应的 Set；先确认等待整轮完成是否覆盖所有首次读取，不能机械地把 Wait 放在每个内层循环开头 |
| producer 每个 region 都独立 Set | consumer 只在整轮开始 Wait 一次 | 该等待是否只保护一部分数据；不能为了让次数相等，把仍会重叠的生产消费边移到循环外 |
| 普通轮次有发布，尾轮进入单独收尾函数 | 收尾前漏 Wait，或收尾内部又重复 Wait | 沿调用链核对尾轮实际发布、首次读取与最后一次配对；单轮通过不覆盖跨轮和收尾 |

例如，只在多 region 时卡住而单 region 通过，先比较两者展开后的配对与可达路径；只在后续轮出现旧值，则比较发布/回收对应的 region 和 generation。增加 event 数字或延长超时不会自动修复这些关系。超时本身也不能证明事件配对有误，仍需保留最后有进展的阶段、可达发布点和相邻 control。

展开嵌套循环时，以 SetFlag/WaitFlag 的配对要求检查事件类型与轮次；目标为 CANN 9.0.0 时，调用 CANNBOT `ascendc-docs-search` 技能取得该版本要求。是否可在循环外等待，仍取决于当前工程的依赖关系与事件族的计数、广播语义。

### 先证明同步对象保护了真实数据区域

对每条可疑 producer→consumer 边，先建立一行区域账本：本轮 generation、producer 实际最后写入的 region（物理根/基址、offset 与有效范围）、consumer 第一次读取的 region、同步对象选择的 slot/index/generation，以及这两个动作所在的实际执行路径。只有区域和 generation 均能由地址或可复核映射确证为匹配对象，且 producer 的有效写入覆盖 consumer 的首次读取，Set/Wait 配对才从候选升级为 “保护了这条数据边”的证据。

变量同名不能替代该核对：index 可以在另一条流水上重新绑定；同一根 buffer 的不同 offset 仍是不同 region；表达式写法不同也可能在当前取值域指向同一 region。例如，`idx & 3` 与 `idx % 4` 在非负整型索引的相应取值域可等价，不能仅因文本不同判为错配。反过来，producer 写 `(idx * TILE)` 而 consumer 读 `((idx - 1) * TILE)` 时，若两侧都引用 `idx`，仍须按实际轮次展开。宏、alias、类型转换或循环范围使映射无法确定时，结论为 `UNKNOWN`；用实际地址、region 标识和轮次探针区分，不以名称或正则匹配替代证据。

若已证明同步对象保护的是另一 region 或另一 generation，优先回查 producer 实际发布点、consumer 首次读取点和索引重绑定处；不要先改 event 数量或插入全局停顿。静态工具对此类关系只能提供候选：它的 finding 需要地址/轮次验证，未命中也不能证明映射正确。

生产 Cube→Vector ping-pong 代码提供一种实现观察：producer 只在结果写入共享 workspace 后发布，consumer 每轮等待对应 generation；buffer 将被复用前，consumer 再沿反方向确认完成。诊断时把两个方向、共享槽位、循环 generation、发布流水和等待条件分别记账。跨核通知不替代核内流水依赖，也不能从某份实现的 flag 数字、轮转深度或 PIPE 参数外推当前工程合同。

当发布藏在可跳过的装载/计算条件中，或 slot 在消费者结束前将被复用时，按 [同步诊断：保护区域、条件路径与缓冲回收](../cases/sync-dataflow-handshake.md) 的调查分叉分别验证“等待仍可达但发布被跳过”和“消费结束前覆盖”；前者关注路径是否闭合，后者关注回收依赖的位置，不能由一次 barrier 的通过合并为同一根因。

最小实验是只延迟 producer 发布或只隔离一个 workspace generation，并预声明旧值应在哪轮消失；同时保留尾轮和一个 producer/多个 consumer control。若全局 barrier 通过，只说明时序相关，仍需区分过早发布、错误 generation、地址重叠和核内可见性。

同步插桩的观察点也必须有数据就绪边：写明 producer 的完成位置、消费者第一次读取位置、共享 buffer/generation 与预计依赖。额外 barrier、dump 或强制单核可能同时改变时序、tile 复用和 core 调度，故只能产生候选信号。采用 `Parent → probe → Parent` 回放；若两侧 Parent 的失败率或错误坐标不稳定，先报告分布与扰动，不把 probe 的通过解释为某个同步 API 已修复问题。

## 接口使用中的同步遗漏

TPipe/TQue 路径在 AllocTensor 后直接消费、漏过 EnQue/DeQue 交接，或手动路径少了一条事件依赖，都可能读到尚未完成的搬运结果。先确认当前编程形态下由谁建立就绪边，再检查真实读写流水；仅有 `PipeBarrier<PIPE_V>` 不能据此说明已保护 Vector 写与搬出之间的跨流水消费。

相同事件连续发布、条件分支多等一次、两条搬运写同一区域时，按首轮、中间轮和收尾展开数据就绪与缓冲回收；不要机械要求每个循环内部 Set/Wait 相邻。合法跨轮配对和条件遗漏见[同步案例](../cases/sync-dataflow-handshake.md)，具体事件、保留 ID 和接口支持调用 CANNBOT `ascendc-docs-search` 技能查当前原文。

## 缓存可见性

GlobalTensor 标量访问与 DMA 可能经过不同的数据通路。地址和生产者结果都正确、消费者仍读到旧值时，分别检查写回、失效和读取的顺序与范围；跨核屏障不能自动替代当前通路需要的缓存操作。多核标量写相邻位置，还要考虑缓存维护单位相交造成覆盖，用分离写区的对照定位。

调用 CANNBOT `ascendc-docs-search` 技能核对 DataCacheCleanAndInvalid、GlobalTensor GetValue/SetValue 的缓存条件与实际搬运通路。批量 DMA 可以作为不同访问路径的对照，但它会改变同步与布局；不能把官方的谨慎使用提示扩大成一律禁用标量访问。

**混合 VF。** 在 CANN 9.1.0 的 950 模型中，Scalar 把 VF 发射到 Vector Function Queue，各 VF 串行执行，并与 MTE 异步工作。SIMD VF 与 SIMT VF 对应不同的计算方式；相邻 VF 的顺序不等于外部搬运已经完成。SIMD Reg VF 内的 Aux Scalar 与外层 Main Scalar 也属于不同执行域，函数标签和允许调用的 API 受此影响。[混合编程模型][vf-source-4]、[Reg 矢量计算编程][vf-source-10]

[vf-source-4]: https://gitcode.com/cann/asc-devkit/blob/e5451c7feba11f9e259f4fbc19c4bf82f8c0496c/docs/guide/编程指南/高级编程/高级AI-Core编程模型/SIMD与SIMT混合编程/抽象硬件架构.md
[vf-source-10]: https://gitcode.com/cann/asc-devkit/blob/e5451c7feba11f9e259f4fbc19c4bf82f8c0496c/docs/guide/编程指南/编程模型/AI-Core-SIMD编程/基于指针的C语言编程/Reg矢量计算编程.md

## SIMT 屏障与参与范围

SIMT 中 asc_syncthreads 的 Block 屏障与 asc_threadfence 的内存栅栏分别核对：前者需要参与线程到达，后者不等待所有线程完成。条件分支或提前返回改变参与集合时，先展开路径，不能只增加同名调用。SyncAll 还需匹配软/硬重载、纯 Vector/Mix 形态、参与核与调度资源；局部屏障通过不能证明全核协议成立。

## 结论限制

SetFlag/WaitFlag、PipeBarrier、核间 barrier 和 Cache 刷新解决的依赖不同，不能互换。API 是否自动同步、event ID 管理和可用流水均是版本与编程范式契约；必须读取当前 CANN 的 API 文档。没有执行环境时只能给出实验计划，不能声明某条同步已经由框架保证。

准备正式处置时，先区分哪些同步仅用于诊断，哪些依赖已由证据支持；撤销未接受的探针，再验证原流水、不同 tile 数和重复运行，并检查死锁、串行化或事件资源泄漏。若打算保留范围更大的同步，需说明其保护的数据边、适用路径和性能代价，按任务要求完成验证；探针通过本身不足以接受该处置或确认具体根因。
