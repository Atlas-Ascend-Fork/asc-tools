# Matmul/MMAD 算子族

## 数学与 reference

C = A×B + bias；实际链路包含 GM/L1/L0 搬运、布局变换、K 维累加和输出搬出。

冻结 batch、M/N/K、A/B 转置、bias/量化/激活、输入与累加 dtype、输出 layout 和有效区。reference 按实际 K 分块和后处理边界提供局部 oracle；高精度整式只用作数学参考，不能替代逐级累加比较。

## I/O 与实际链路

- 冻结输入/输出 shape、dtype、layout、有效区、padding 和 alias。
- 从 host tiling、GM 搬入、局部计算、同步到 GM 搬出逐节点记录实际计算链，不以 API 名替代。
- tiling 分支、多核分区、尾块与特殊值必须进入风险清单。

## Grouped Matmul：先建分组账本，再看数值误差

当普通单组 Matmul 正常，而错误从第二组开始、恰好落在 group 边界、空组会改变后续输出，或 per-token scale 像整体错移时查询本节。不要先把它归成“Cube 精度不足”；先为每组建立一行实际账本：

| 字段 | 必须记录的实际值 |
|---|---|
| 逻辑 shape | `group_id, M_g, N_g, K_g`，以及 shape 来自 host tiling、group list 还是固定字段 |
| A/B/C 视图 | dtype、layout、`transA/transB`、leading dimension、有效区和 padding |
| 物理区间 | A/B/C 各自的 base、实际 allocation span、末地址；不能只写 `M*K/K*N/M*N` |
| 分块 | `baseM/baseN/baseK`、M/N/K tile 数、首尾 tile 的真实有效量 |
| 执行 | group 到 flat task/block 的映射、AIC/AIV 角色、workspace slot/epoch、最终 owner |
| 后处理 | bias、channel scale、token scale、激活和输出 Cast 的索引公式与 dtype |

把三张 prefix sum 分开计算：`A_base[g]=A0+sum(spanA[h], h<g)`、B 和 C 同理；shape prefix、存储 span prefix 和 task prefix 是三种不同量，不能共用一个 `offset`。对普通行主序 ND 的诊断模型，逻辑坐标可先写成：

```text
physA(g,i,k) = A_base[g] + (transA ? k*lda+i : i*lda+k)
physB(g,k,j) = B_base[g] + (transB ? j*ldb+k : k*ldb+j)
physC(g,i,j) = C_base[g] + i*ldc+j
```

这只是核对当前实现的坐标账本，不发布任何 API layout 合同。NZ/分形、batch、压缩或带 padding 存储必须换成对应版本的官方文档证明的物理 mapper；此时用逻辑元素数直接累加偏移属于反模式。即使 `M_g=0`，也要逐 tensor 判断该组是否仍拥有 weight/bias/scale 存储，不能统一把 A/B/C span 都清零。

## GMM 的最小局部 oracle

先缩成单 group、小 M/N/K、单个 K-block，并关闭或冻结 bias/量化/激活；用 A 的单行/单 K one-hot 配合 B 的单位基，令一个 `C[i,j]` 只有一个可精确表示的预期贡献。按 `A物理坐标 → Cube输入 → CO1/workspace → epilogue输入 → C物理坐标` 比较同一逻辑元素。该实验只能证明被激活 group、K-block、转置分支和输出坐标；不能证明第二组、尾组、空组、其它 K-block 或量化索引。

然后每次只增加一个维度：第二个非空组、一个空组、M/N 尾块、第二个 K-block、transpose、非紧致 leading dimension、token scale，最后才恢复完整 epilogue。若加入第二组后首错恰好等于前组 span，优先反解 prefix/leading-dimension 候选；若 CO1/workspace 正确而 Vector 后首次错，再查 scale 索引、workspace epoch 和 Cast。随机大 shape 一次通过不能反证这些边界问题。

具体数组长度、offset 公式、分核公式、同步 flag 和量化 API 必须按当前实现与版本核对。调用 CANNBOT `ascendc-docs-search` 技能查当前 Matmul/Mmad 接口与样例；可变 `M/N/K` 的 GMM 合同、transpose/NZ span 和 leading-dimension 约束缺少该版本权威依据时保持 `UNKNOWN`。

## 敏感 checkpoint

按当前疑点选择少量相关观察点，说明 reference、CPU 或 NPU 观察面及所用参考或检查性质。缺少可靠局部 oracle 时可先保留原值或检查适用的不变量，无法判定的部分保留未知。选点和对照见[观察点设计](../../diagnosis/checkpoint-design.md)；已有可比误差且需要解释累积、放大或抵消时，再用[逐层分析](../../diagnosis/progressive-error-analysis.md)。

可从 L1 输入、L0A/L0B 准备、首尾 K 块累加器、跨块合并和最终搬出中选能区分候选的位置，无需全链采集。使用非对称 M/N/K 和可逆矩阵模式暴露转置或 layout 错误；用小 K/大 K 和强消减输入区分参数、尾块与累加顺序。checkpoint 已错时先追查其输入与计算，修复位置由根因证据决定。

## K-block 参与集合与分段定位

按当前实现串起 `GM 输入 → A1/B1 → A2/B2 → CO1 → GM 输出` 的相关路径，在所选边界比较对应的逻辑数据；调用 CANNBOT `ascendc-docs-search` 技能取得目标版本的矩阵装载与 Matmul/Mmad 原文，再核对具体调用、布局和字段。

- A1/B1：确认输入有效区是否完整进入 L1，并能由坐标编码反解回原逻辑位置。
- A2/B2：按实际 ZZ/ZN/NZ 分形比较坐标，不把两个不同布局的线性 dump 直接逐元素相减。
- CO1：用输入控制分别计算各 K-block 的预期贡献，再比较全部 block 同时参与时的结果。
- GM：确认写回后的逻辑坐标、输出 dtype 和有效区；CO1 局部通过而 GM 失败时，先区分写回新增差异与前段偏差的放大，不能仅凭局部通过排除 Mmad 相关路径。

当 K 被切成多个 block 时，先证明参与项集合，再讨论累加 dtype 或顺序。逐个只激活一个 K-block、其余 block 置零，并增加全部 block 同时激活和全零 control；各组使用非对称、可逆且能在载体 dtype 中精确表示的坐标编码。某一块的预测贡献消失，只能先把边界缩到该块的搬入、布局或参与集合，不能仅凭最终误差宣布 Mmad 精度不足。

分段判断遵循以下顺序：A1/B1 首先异常时查 Nd2Nz 的 shape、stride 和 padding；A1/B1 正常而 A2/B2 异常时查 LoadData 的分形、transpose、repeat 和 stride；A2/B2 正常而 CO1 异常时查 `m/n/k`、C 初值、K-block 参与集合和合法 dtype 组合；CO1 正常而 GM 异常时查 Fixpipe 的 format、`m/n`、stride、量化和输出 dtype。有限数的布局与参与集合通过后再测试特殊值；NaN/Inf 的具体传播必须来自目标算子合同或独立 oracle，本页不定义该语义。

## A/B 布局指纹与后处理隔离

只用随机矩阵容易让 A、B 转置、分形错位和漏 K-block 得到相似的稠密误差。先设计能给竞争候选不同预测的输入：让 A 的单个逻辑行或 K 位置使用 one-hot/basis 模式，让 B 使用 identity 或非对称坐标编码，再交换两侧角色复测。每个编码值必须能在输入 dtype 中精确表示；对称矩阵或重复常量只能作为全零/全通 control，不能排除转置和错位。单位矩阵通过也只证明被激活的路径，不证明其他 K-block、尾块、bias 或输出布局。

需要区分主计算与 epilogue 时，优先在原路径中比较 CO1 与相关后处理边界。关闭 bias、激活、量化、Cast 或写回转换属于实验干预，优先控制一项，并核对 overload、tiling、dtype 与布局的联动；一次关闭多项按组合观察解释。CO1 局部通过、后处理后首次超差时，先用实际 CO1 输入重放可信后处理，区分本段新增差异与上游偏差放大。局部容差通过或局部重放一致不能排除上游原因；后处理若能解释全部变化，应回查 K-block、累加精度或运算顺序。CO1 无法安全观察且没有等价局部边界时保留 `AMBIGUOUS`，不从最终输出反推它正确；关闭后处理后误差变小也不证明主计算正确。

每次比较都记录 `逻辑坐标 → 当前存储级坐标 → 预期贡献`。首个 K-block、共享维后半段、最后完整块和合同允许的 tail 应分别有可区分输入；只有逐级存储边界显示某一贡献首次消失，才进入对应搬入、布局或参与集合候选。one-hot、坐标编码和分块激活用于区分候选；API 字段、布局支持表、对齐值与精度组合以当前版本 API 页为准。

## 装载布局与 scale 配对

换成 fp8/fp4 后出现 NaN、首个 N-segment 正常而后续出错，先分别推导数据与 scale 的地址。逻辑元素数、打包载体和字节数不是同一个单位；fp4 尤其不能把载体 sizeof 当作单个逻辑元素大小。逐个 K-pass、N-segment 记录两类地址与有效范围，调用 CANNBOT `ascendc-docs-search` 技能核对所用装载接口的字段单位。

multi-head 只有奇数或偶数 head 异常时，分别比较单 head、双 head 的物理起点、容量和生命周期。数据与 scale 可能有不同的布局与打包方式；每一类都应独立推导偏移，不能因为症状相似就套某个 head-offset 公式。共享区还要检查上一 head 是否已经消费结束，见[多层分段状态](../mechanisms/tiling-tail-state.md#多层分段状态rowheadn-segment-与-k-pass)。

UB→L1 的注册或同步异常，先确认目标产品支持当前接口，再分开核对数据搬运与通信。CANN 9.1 DataCopy_UBToL1_continuous 文档描述的 950PR/950DT SSBuffer 方式使用 SSBuffer 通信，数据经 UB→L1 硬件通道直搬；文档另说明经 Matmul workspace GM 中转并需 REGISTER_MATMUL 的路径。实际采用何种方式须结合 ENABLE_CV_COMM_VIA_SSBUF、执行形态与编译分支核对：配套源码中 SSBuffer 内部开关的推导及直搬分支均涉及 `__MIX_CORE_AIC_RATION__`，用户开关为 true 或 false 都不能单独证明通路。其他产品按对应支持范围与路径查原文，不支持的产品不能靠补注册获得能力。路径切换涉及工作区与同步变化，分别观察；注册成功不等于精度通过。ND2NZ 的补齐长度、零步长、b4 参数和转置连续性也按所用重载核对，保持逻辑有效 K。

## 计算与写回中的常见分叉

| 现象 | 先核对什么 |
| --- | --- |
| 改转置参数后整体错位 | 编译期转置能力、运行时请求、Host tiling 与真实布局是否匹配；支持转置不等于每次都转置 |
| 异步写 UB 后偶发旧值 | 当前 GetTensorC/Iterate 路径是否完成了所需等待，再开始 Vector 消费；同时检查上一轮的回收 |
| 输出全零或整块未更新 | Iterate 与结果获取是否配合，自管 CO1 路径是否实际搬出；零任务按原空输入语义处理 |
| half 输入精度超差 | 区分内部累加和最终写回类型；基础 Mmad 的 310p AI Core/910 特定组合提示不能套到所有高阶 Matmul |
| 只有 M=1、特定 K 或小块失败 | GEMV、K 对齐或相邻 Mmad 依赖是否改变实际路径；先展开调用与地址，不套历史阈值 |
| CO1 正确、最终输出错位或变值 | Fixpipe 的目的布局、转换/量化与有效范围；特殊值按原输出要求比较 |

调用 CANNBOT `ascendc-docs-search` 技能查实际矩阵计算接口的类型组合、异步完成与搬出规则。int8/int4/MX 路径分别核对累加、Bias、模式配置和写回类型，不能从最终 C 类型倒推内部计算。

## HF32 与中间精度

fp32 矩阵乘误差偏大时，可在其他条件不变的情况下关闭当前接口支持的 HF32 开关做对照。误差改善只支持精度路径相关的解释，还需比较局部输入输出、算法和实际生效状态，不能直接认定根因。Host、框架、Kernel 三处的配置要和每次局部参考一起记录；开关无效果时检查配置时机与上层覆盖。

调用 CANNBOT `ascendc-docs-search` 技能核对各 HF32 控制入口的枚举取值、关闭能力和执行形态，不能照搬 mode 数字。是否接受降精或改用另一计算路径，由原输出要求决定。

HF32、SetMadType 和 C 接口的 asc_set_fp32_mode 分别确认使能方向和支持条件，不能按相似名称互换。CANN 9.1 SetMadType 中存在 HF32 枚举但标为暂不支持的说明；查询具体控制入口后，再用固定输入与局部观察确认模式真正生效。

C 接口的 asc_mmad_mx 需跟踪 C 清零、初值选择与重复调用是否延续 L0C；asc_set_l0c2gm_config 等搬出配置可能继续执行量化、激活或格式转换。把这些状态放入现有 CO1→输出的观察链，不把最终输出类型当作内部精度。

## GMM 的状态与量化后处理

每组进入计算时核对实际 singleM/N/K。采用 `mList[0]==-1` 和 groupList 前缀的实现，组边界改变后仍用旧 M，可能使跨组行错位；按当前接口定义重建分组范围，不能把这套字段约定推广到所有 GMM。

[GroupedMatmul 官方案例](https://gitcode.com/cann/asc-devkit/blob/e5451c7feba11f9e259f4fbc19c4bf82f8c0496c/docs/guide/算子实践参考/优秀实践/GroupedMatmul算子性能调优案例.md)面向 CANN 9.1.0 资料中的 Atlas A2 per-token 量化路径。整数中间类型、缓冲份数与核配比属于该例的组织方式。当前实现应分别确认 AIC 写完、AIV 开始读、AIV 消费结束和允许复用四个位置；缓冲少本身不证明有竞争，增加份数也不能替代同步。

CO1/workspace 正确但后处理错误时，分别检查 channel scale、token scale 的逻辑索引和 Cast 时机。用两组不同的 scale 哨兵区分广播方向；整数结果过早转低精度后，信息可能在乘 scale 前就丢失。FP32 反量化或有序合并都需要保持原算法、溢出与验收要求；int32 方案仅用于已确认的整数累加路径，不把浮点 GMM 直接改成整数计算。

GMM 更新时分别核对 SetSingleShape 的当前计算范围与 SetOrgShape 的物理组织，组间偏移不能只由同一个 M/N/K 三元组推导。同一对象复用还要确认上一组结果消费完成；官方案例中的 scale、累加类型和核配比仅适用于其自身场景。

## 版本路由与边界

矩阵装载与 Matmul/Mmad 的接口资料调用 CANNBOT `ascendc-docs-search` 技能获取。Mmad 参数、矩阵格式和对齐要求应根据所查官方页面并结合当前实现核对；本页不代表任一具体实现或 NPU case 已验证。
