# 共存边界判定规则（B1/B2）

分流表出现多个候选、与决策树结论冲突、命中「依 B1/B2 判主型」，或工具结果需要重新分流时，读取本文件；候选明确且无矛盾时不必读取。

### 工具结果回流分流表

根据工具或 plog 的新证据定位候选主型，再按对应的 B1/B2 规则核对判定条件。下表只负责路由，不替代具体判据。

| plog/工具证据形态 | 转向候选主型 | 判定依据（锚点） |
|-----------------|-------------|----------------|
| comm config 参数不一致（各 rank 值互异且均合法有效） | `collective-param-mismatch` | B1「值类参数核对次序」第 4 条（其余值互异） |
| 各 rank collective 调用 comm 句柄不同域，错配侧 nullptr/`HCCL_E_PTR`/`comm name already exists` 即时失败 | `collective-param-mismatch`（comm 句柄跨域不一致型） | B1「comm 句柄错配族判定次序」——错配域 init 成功且 collective 在错域 no-op 自洽完成才归 `single-rank-block`（会话/句柄错配型） |
| 单 rank 传零元素/空值退化参数被入口拒绝，其余 rank 等待超时 | `single-rank-block`（调用退化型） | B1「值类参数核对次序」第 1 条（数值退化值） |
| 值类参数为溢出/超界非法值（近类型极值/超出 API 或缓冲契约域，单侧即不可能合法；含单 rank 条件注入形态） | `comm-operator-anomaly`（值域违反/参数域契约违反型） | B1「值类参数核对次序」第 2 条（值域违反；全 rank 对称形态见第 6 条） |
| 错配目标位于表驱动映射构造（批次 item 表/对端基/peer 表，典型陈旧拓扑残留） | `rank-group-mismatch`（映射表错配型） | B1「值类参数核对次序」第 3 条表驱动 bullet |
| P2P 调用点标量目标越界（dstRank/srcRank 指向不存在对端） | `comm-operator-anomaly`（寻址域违反） | B1「值类参数核对次序」第 3 条 P2P bullet |
| 各 rank 调用次数/同组 collective 顺序错位（编排分歧） | `communication-timeout`（编排不一致型） | B1 主型表「编排」行——限集合调用序列本身错位；P2P 配对缺失致某 rank 阻塞在集合调用之前的点对点等待点→ `single-rank-block`（P2P 配对阻塞型） |
| 对端 rank 缺失（未参与/提前退出/阻塞/资源错用） | `single-rank-block`（按对端形态细分：提前退出/死循环/host 长阻塞/调用退化等；崩溃退出、显式销毁后跳过、过早销毁、建链失败→ `communication-timeout`） | B1 主型表对端形态各行（多条命中取与源码/日志证据最具体匹配的形态行） |
| stream/event 等待环，或 prime/Reset 后无再 Record 的永久阻塞 | `stream-event-cycle` | B2 证据形态表 + B2 判定次序——个别 rank 错用致缺席先查 B2 适用前提（分流至 B1）；Reset 后存在可达再 Record 的单向永久阻塞→ `sync-imbalance`（event 生命周期陷阱） |

### 共存边界主型归属规则（歧义消解；优先于分流表候选排序与决策树粗分流）

> 两类语义相邻边界的判定依据。当分流表多候选并存或与 method/decision-trees.md 粗分流结论冲突时，以本节规则为准。

**B1「等待端通信超时」×「对端缺席/异常」共存**——等待端出现 `watchdog timeout`/`hcclExecTimeOut`/`HCCL_E_TIMEOUT`/`waiting for rank` 等超时信号，同时存在"某 rank 未参与/提前退出/阻塞/资源错用"。主型按**对端（缺席/异常 rank）形态**判定，而非按"是否出现超时报错"（等待端超时日志只证明主现象，不单独决定主型）：

| 对端形态 | 主型 |
|---------|------|
| 崩溃退出（abort/SIGABRT/core dumped 等异常信号终止） | communication-timeout（崩溃缺席为根因，等待超时为主现象） |
| 跳过 collective 但**显式销毁通信域**后退出（该 rank 退出路径调用通信域销毁接口收尾，销毁证据见下方注） | communication-timeout（参与编排不对等） |
| collective 前过早销毁通信资源（**单 rank 条件分支**过早销毁 comm/stream，该 rank 调用失效/不再有效参与，其余 rank 正常参与并在通信等待点超时——存在"等待端-缺席端"结构） | communication-timeout（资源生命周期错误）；注意区分：销毁后继续以失效句柄调用且为**全 rank 对称**（无 rank 条件、双侧同形态、全部调用失败，无对端等待结构）→ comm-operator-anomaly（use-after-destroy API 契约违反） |
| 建链/init 失败或超时，根因为 device 使用错误（多 rank 同卡争抢/硬编码同卡） | communication-timeout（建链分支；通信域参数不一致才判 rank-group-mismatch） |
| 各 rank 以不一致的**编排**参与同组**集合通信**（迭代次数分歧：rank0 多轮/rank1 少轮；或同组 collective 调用顺序相反），各自调用序列自洽但相互错位，某集合点调用错配等待超时 → **限集合调用序列本身错位**（次数/顺序互异）；P2P 点对点调用（Send/Recv）配对缺失/错配致某 rank 阻塞在集合调用**之前**的点对点等待点（该 rank 未到达集合调用，其余 rank 等待超时）→ 不属编排分歧，按对端"到达前阻塞"判 single-rank-block（P2P 配对阻塞型） | communication-timeout（编排不一致型） |
| host 侧有限时长长阻塞（sleep/IO 阻塞，之后才到达调用点） | single-rank-block（host 长阻塞型） |
| 永不推进（死循环/无限等待，host 栈停在业务代码） | single-rank-block（死循环型） |
| 绕过通信域销毁直接退出进程（退出路径未调用通信域销毁接口即 return/exit，通信域句柄被遗弃、交由进程退出自动回收；已释放本地计算资源或已做 ACL 运行时级收尾〔ResetDevice/aclFinalize〕不改变本形态；错误被吞掉后的**假成功退出〔exit 0〕**同属本形态——**先判别**：算子为自研 kernel 编排引擎通信算子且"假成功"侧 op 实际完整返回〔ret=0、本端数据校验通过、返回耗时异常短〕而非跳过调用时，该侧不是缺席方，属 [comm-engine-protocol-fault](symptoms/comm-engine-protocol-fault.md) 通知原语类别错用的假成功形态〔完成等待被误发通知提前解除〕，不判 single-rank-block） | single-rank-block（提前退出型；自研 kernel 编排引擎假成功形态除外→ comm-engine-protocol-fault）（内部第九类；报告 §1 按固定八类归档：通知停滞归 `stream-event-cycle`，通信完成后数据损坏归 `comm-operator-anomaly`；§4 根因记录“用户引擎协议层”和具体机制） |
| 到达调用点但调用退化、未构成有效参与（**值类集合参数**的零元素/空集合等**退化值**——count=0、空 recvCounts 等（值本身无有效语义）——被入口检查拒绝而即时失败退出，或零元素被视作 no-op 正常返回，两种结局均为"该 rank 未构成有效参与"，其余 rank 等待超时。**限值类参数且限零/空/nullptr 退化语义**：溢出/超界非法大值（值域违反）不属本行——按「值类参数核对次序」第 2 条判 comm-operator-anomaly（参数域契约违反型）；资源句柄（stream/comm/event）的 nullptr/失效调用不属本行——comm 句柄失效按下方句柄错配族分型；stream 等执行资源句柄因**过早销毁**失效 → 上方"过早销毁"行（communication-timeout）；其它资源会话错用 → 会话/句柄错配行） | single-rank-block（调用退化型） |
| 各 rank 传给**同一 collective 调用**的 comm 句柄指向**不同通信域**（worldSize/成员不一致，如一 rank 用共享域 world=N、另一 rank 用 solo 域 world=1 或额外域 init 失败后的 nullptr 无效句柄），且错配侧 collective 调用**即时确定性失败**（入口参数检查报错 `ptr [comm] is nullptr`/`HCCL_E_PTR`、额外域 init 报 `comm name already exists`、`HCCL_E_PARA`）并以错误退出 | **collective-param-mismatch**（comm 句柄跨域不一致型——collective 调用的 comm 参数在 rank 间不一致；优先于本表 single-rank-block 各行判定） |
| 通信会话/句柄错配（中途切 device / 对未 Record 的 event Wait；或用错误 comm 句柄但该错配域 init **成功**、collective 在错域**自洽完成**〔world=1 no-op 正常返回〕，该 rank 无调用级报错、生命周期正常收尾） | single-rank-block（会话/句柄错配型：到达但未有效参与） |

> B1 多条命中时，取与源码/日志证据最具体匹配的形态行判定。
>
> **comm 句柄错配族判定次序**（"用错误 comm 句柄"类证据出现时）：先核对各 rank 传给同一 collective 的 comm 句柄是否同域（源码 collective 调用的 comm 实参 + plog 各通信域 init success/failed 条目）——句柄跨域且错配侧**即时失败**（域 init 失败/nullptr → `HCCL_E_PTR` 等参数级报错）→ **先判 collective-param-mismatch**（comm 句柄跨域不一致型）；仅当错配域 init 成功、collective 在错域自洽完成（no-op 正常返回、正常收尾）才判 single-rank-block（会话/句柄错配型）。
>
> **值类参数核对次序**（count/dataType/op/root 等值参数出现"不一致/非法"表象时，在 comm 句柄核对之后执行；按**参数语义类别**分型，不看该侧调用是入口即拒还是等待超时——两类结局在各类别中都可能出现，无区分力）：
> 1. **数值退化值**（count=0 零元素/空集合/空指针 buf——值本身无有效语义）→ 该 rank 调用退化、未构成有效参与（被入口检查拒绝即时报错退出，或被视作 no-op 返回，两种结局同型）→ **single-rank-block（调用退化型）**，退化参数作为该 rank 根因记录（修复语义 = 恢复有效参与：统一为合法值）
> 2. **值域违反**（参数值超出 API 合法域或算子/缓冲物理契约域——整型回绕/近类型极值的溢出探测值、count 远超 sendBuf/recvBuf 实际容量、超出 API count 上限；单 rank 视角即可判定"该值不可能合法"，通常被入口参数检查拒绝〔`HCCL_E_PARA`〕确定性失败、无部分完成数据）→ **comm-operator-anomaly（参数域契约违反型）**——**无论单 rank 条件注入（其余 rank 正常参与并等待超时）还是全 rank 对称同非法值（全部调用失败、无等待效应）**，rank 条件形态不改判 single-rank-block / collective-param-mismatch；值常源自算子链路上游计算派生（换算溢出/陈旧规模常量越界），修复语义 = 在参数派生源恢复契约合法值（非恢复参与、非跨 rank 对齐）。与第 1 条分界：退化值限零/空/nullptr（无数据可通信的参与退化）；溢出/超界大值是"以非法载荷调用"，参与意图存在。与第 4 条分界：第 4 条各 rank 取值均在合法域内（互异或与契约不符但仍可被接受执行，常致部分完成/协议层等待）
> 3. **目标/寻址类参数错配，按参数载体分派**（值有语义但指向错误/不存在的对象）：
>    - **集合调用点标量寻址参数**（各 rank 传给同一集合调用的 root/srcRank 等标量实参）越界或互异 → **collective-param-mismatch（配对/寻址错配型）**，无论该侧调用是入口即拒（如 `HCCL_E_PARA` 立即返回）还是挂在等待直至超时——修复语义 = 纠正配对/寻址参数（指向真实存在的对端），非恢复参与，故不改判 single-rank-block
>    - **P2P 点对点调用标量目标参数**（HcclSend/HcclRecv 的 dstRank/srcRank——单侧调用的算子契约参数）越界指向不存在对端 → **comm-operator-anomaly（寻址域违反，与第 2 条同族）**——单侧算子契约违反，无"跨 rank 一致性"维度可违
>    - **表驱动参与者映射构造条目错配**（HcclBatchSendRecv 类批次的 send/recv item 表、对端基/peer 映射表、分组/路由表——定义"谁与谁配对"的结构性映射数据；典型为陈旧拓扑残留：按旧 rank 数/旧对端基生成的表项未随当前通信域规模更新）→ **rank-group-mismatch（参与者映射表错配型）**——错在跨 rank 配对映射结构（对端无对应视角项→配对失败/无对端等待），非单点调用参数；通信域 init 本身成功（区别于 rank-group-mismatch 协商阶段形态）
> 4. **其余值互异**（各 rank 所传值均合法有效、仅取值不同，进入协议层后失配暴露——timeout/`CMD information check fail`/配对失败）→ **collective-param-mismatch**
> 5. **分块类参数（如 AlltoAll 的 per-destination 块大小）两步判**：① 先按**单 rank 视角**核对该 rank 的算子语义约束——**各目标分块之和是否等于全局输入张量第 0 维（各 rank 一致的业务张量维度/元素总数，如 elem_count；不是各 rank 自行按分块截断分配的 sendBuf 大小）**、缓冲区是否容纳 分块×rankSize——**任一 rank 单侧违反 → comm-operator-anomaly**（算子契约违反，无需 rank 间对比）；② 各 rank 均自洽（分块之和均等于全局张量维度）、仅块粒度互异 → collective-param-mismatch
> 6. 与 comm-operator-anomaly 的分界：调用失败参数若为**全部 rank 共同违反** API 契约（无 rank 条件分支、所有 rank 以同样非法值调用、全部调用失败报错，无"其余 rank 等待"效应）→ comm-operator-anomaly（第 2 条值域违反的全 rank 对称形态）；仅**单 rank 条件分支**传**零/空退化值**、其余 rank 正常参与并等待 → single-rank-block（调用退化型，第 1 条）。
> 7. **复合核查（单根因确认前必做）**：判出单一主型前，扫描 driver 源码的**全部独立异常注入点**（rank 条件分支/FAULT-ANOMALY 注释段/非对称三元选择，grep 可得；**扫描范围含 driver 引用的关联源文件**——通信参数表/查表函数文件、头文件常量等跨文件供给源中的 rank 条件取值分歧同样是注入点，且主根因未修前可无症状）——存在 ≥2 个**不同层**（算子契约 / 集合参数 / 编排 / event 生命周期）的注入点，且单一根因无法解释全部证据（如既有调用级错配证据、又有 event/stream 层永久阻塞证据）→ **composite**（各分量按对应单一类型规则独立鉴型）。
>
> **「通信域销毁与否」核验方法**（判"显式销毁后跳过"与"绕过销毁直接退出"两行时必查）：核对缺席 rank 的**显式销毁调用证据**——plog 中该 rank 是否存在通信域销毁调用的发起/完成条目（如 `Entry-HcclCommDestroy` … `comm destroy complete`），或 driver 源码中该 rank 的退出/收尾路径是否包含通信域销毁调用。**进程退出期的 transport 去初始化/设备 close 等清理日志属进程退出自动回收，不构成"显式销毁"的证据**——缺席 rank 无销毁调用条目却出现退出期清理日志时，仍判"绕过通信域销毁直接退出"（single-rank-block），不得据清理日志改判"销毁后跳过"。两行均要求销毁动作与"是否继续调用 collective"联合判读：先销毁再继续编排 collective → 过早销毁行；销毁后不再调用 collective 即退出 → 显式销毁后跳过行。

**B2「跨 stream event 同步问题」边界**——计算流↔通信流/主流↔子流之间发现依赖或 event 使用问题。主型按**依赖边存在性与 event 生命周期状态**判定，不按"hang 与否"单独判定：

> **B2 适用前提（先决条件，先行核查）**：B2 分型适用于——① 无 rank 缺席结构的现场（各 rank 对称地卡在 runtime 同步点/数据错误，或单进程内双 stream 编排问题）；或 ② stream/event 错用为**全 rank 同源对称**（所有 rank 执行同一代码路径、双侧同形态）。若 event/stream 错用仅发生在**个别 rank**（rank 条件分支注入）且导致该 rank 缺席后续 collective、其余 rank 在通信等待点等待超时，**按错用性质分流**：① **单纯同步原语使用错误**（对从未被 Record 的 event 调 StreamWaitEvent——源码中该 event 无任何 Record 调用、无环、无 Reset 生命周期操作，属漏配/错用原语）→ 不进入 B2，按 B1「同步原语错用型」判 single-rank-block；② **依赖图拓扑成环**（双向/多 stream 循环 Wait、环内可含 collective 调用——缺席是环的下游效应）→ 仍进 B2 判 stream-event-cycle（等待环型，环是 runtime 层根因）；③ 切 device/错 comm 句柄等会话错用 → B1 会话/句柄错配型。

| 证据形态 | 主型 |
|---------|------|
| producer/consumer 跨 stream 边界**依赖边完全缺失**（应 Record+Wait 而未建立）→ 数据竞争/读到未就绪数据/结果错误（非 hang，进程可正常退出） | stream-event-cycle（依赖缺失型） |
| event **跨轮/跨阶段复用未 Reset**（上一轮残留 signaled 状态 → 本轮 Wait 提前/立即返回，读到旧数据，结果错误，非 hang） | stream-event-cycle（状态残留型） |
| 依赖图成环（双向/多 stream 循环 Wait）→ 静默 hang | stream-event-cycle（等待环型） |
| Wait 的 event 本轮永不会被 Record（Wait 先于本轮 Record 且 Record 方被阻塞；或 event 被 Reset 后**再无任何 Record**、或再 Record 不可达——排在被本 Wait 阻塞的段之后）→ 静默 hang | stream-event-cycle（次序错配型） |
| 依赖边存在且方向正确，且 Wait 目标 event 在 Reset 之后**存在可达的再 Record**，但 Reset 预处理（prime: Record+Sync+Reset）/迭代间多余 Reset/错误 stream 上 Reset 破坏跨 stream 可见性 → 单向 Wait 永久阻塞（hang） | sync-imbalance（event 生命周期陷阱） |
| 误用 SynchronizeStream/粗粒度串行化替代 event 依赖 → 性能坍塌（结果正确）或 host 阻塞（含 host 强同步 + 生命周期损坏的 event 组合） | sync-imbalance（错等/过度串行化） |
| **单轮内 Record 时机/位置错误**（依赖边与配对齐全，但完成标记在生产行为**之前**过早 Record / Record 位置过粗 → 下游 Wait 立即通过、读到未就绪数据，结果错误非 hang） | sync-imbalance（时序错位/标记过早型；与"跨轮复用未 Reset"的状态残留型区分——后者是跨轮状态未清理，本行是单轮内时序错误） |

> B2 判定次序：先核查适用前提（个别 rank 错用致缺席 → B1 single-rank-block，不进 B2）→ 判等待-发布对所在层级（等待/发布为**自研算子内的引擎通知协议原语组合**〔host 编排引擎：主机端单边写 + 分离式事件通知；kernel 编排引擎：设备侧 kernel 数据面编排——AI CPU+TS 载体〔任务调度提交 + 通道/线程通知握手〕或 AIV/Vector Core 载体〔HCCL Buffer 软同步 flag 协议 + 完成等待以流同步表达〕，均看机制类别、不依赖原语库符号名〕→ [comm-engine-protocol-fault](symptoms/comm-engine-protocol-fault.md)（通知链路断配方，含 kernel 载体的完成等待超时形态——AI CPU+TS 载体 host 回发等待失败行 / AIV 载体 kernel 下发后流同步超时——与假成功形态），不进 B2 或 B1（内部第九类；报告 §1 按固定八类归档：通知停滞归 `stream-event-cycle`，通信完成后数据损坏归 `comm-operator-anomaly`；§4 根因记录“用户引擎协议层”和具体机制，见[报告归档映射](symptoms/comm-engine-protocol-fault.md#报告归档映射当前接口固定为-8-类)与 [SKILL.md §5](../SKILL.md#5-把结论讲给用户听)）；设备侧 stream/event 原语方进 B2）→ 查依赖边是否建立（未建 → cycle）→ **查是否误用 SynchronizeStream 替代 event 依赖（host 强同步 + broken event → sync-imbalance 类型 B）**→ 查 event 复用 Reset 状态（**跨轮**复用未 Reset 残留 → cycle 状态残留型；**单轮内** Record 时机/位置过早 → sync-imbalance 时序错位型；多余/错位 Reset 致单向永久阻塞且 Reset 后有可达再 Record → sync-imbalance）→ 查依赖图拓扑（成环 / never-recorded → cycle）→ 查串行化方式（SynchronizeStream 误用 → sync-imbalance）。
>
> **「Reset 破坏可见性」与「never-recorded Wait」的重叠消解**（prime/Reset + 单向 Wait 永久阻塞时必查）：**先判 host 同步编排**——若现场存在以 host 侧 `aclrtSynchronizeStream` 做计算流↔通信流衔接的编排（典型证据：collective 下发前后对计算流/通信流的 SynchronizeStream 成对出现于编排路径，即用 host 强同步替代跨 stream event 依赖）→ **主型 sync-imbalance（类型 B 错等/过度串行化）**——同步方式错误是编排层根因，broken event（Reset 后无再 Record 的 Wait）为其伴生；仅当现场为**纯 event 状态机错误**（无跨 stream host 同步编排主题）才继续按下述再 Record 可达性分型。核对 **Wait 目标 event 在 Reset 之后是否存在可达的再 Record**（读源码序：Reset 之后该 event 是否还有 `aclrtRecordEvent` 调用，且该 Record 未被本等待阻塞——位于其它 stream 上、或 host 提交次序先于会被阻塞的段）：
> - **无任何再 Record**，或再 Record 虽存在但排在（直接或间接）被本 Wait 阻塞的 stream 段之后（如与 Wait 同 stream 且 Record 后于 Wait、或 Record 方 stream 又反向 Wait 本侧形成环）→ 该 Wait 的满足条件**永不可达**（等待一个"本轮永不会被 Record 的 event"）→ **stream-event-cycle（次序错配型/等待环型）**；
> - **存在可达的再 Record**（host 已提交、位于不受本等待影响的 stream 段）→ 依赖边的建立意图与 Record/Wait 配置完整，仅生命周期操作（prime 预处理位置/迭代间多余 Reset/Reset 执行在错误 stream）破坏初始可见性 → **sync-imbalance（event 生命周期陷阱）**。
> 判据证据源：driver 源码中同一 event 的 Record/Reset/Wait 调用全序列（grep 提取）+ host 提交次序。
