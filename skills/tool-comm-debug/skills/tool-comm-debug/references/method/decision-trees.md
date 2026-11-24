# 诊断决策树

> 从症状到根因的文本决策树，覆盖 7 类异常 + comm-engine-protocol-fault（自研通信算子引擎协议层）的分支判定。配合 [log-keyword-patterns.md](log-keyword-patterns.md) 与 [rank-localization.md](rank-localization.md) 使用。

## 入口：先判故障层（第一分叉点）

```
现象入口
├─ 有报错（wait timeout / HCCL 错误码 / 进程非0退出）
│   └─ HCCL 层或进程层 → 进入【树 A：报错型异常分流】
├─ 无报错但 hang（进程不退出、无输出）
│   ├─ host 线程停在 aclrtSynchronizeStream/Event？
│   │   └─ 是 → runtime 层 → 【树 B：stream/event 等待环】
│   └─ 否（停在 HcclAllReduce 等通信调用）
│       └─ 进程层 → 【树 A：报错型异常分流】（最终被 watchdog 超时）
│   （注：等待对象为自研通信算子内的引擎通知原语、且全程无原语报错——
│     host 编排引擎静默停滞 / kernel 编排引擎完成等待超时（AI CPU+TS 载体 host 回发
│     等待失败行 / AIV 载体 kernel 下发后流同步超时〔vector core timeout 类〕）——
│     → comm-engine-protocol-fault 症状文档，不进树 A/B）
└─ 数据错误（进程正常退出，结果错乱/偶发数值异常）
    └─ → 【树 C：数据正确性异常分流】
```

## 树 A：报错型异常分流（HCCL 层 + 进程层）

```
报超时 / 集体通信异常
│
├─ 超时发生在哪个阶段？
│   ├─ 建链阶段（init_process_group 前后 / plog 含 socket/connect 失败 / EI0006）
│   │   └─ 建链超时 → 检查：
│   │       ├─ rank 启动一致性（rank_table/RANK_ID/RANK_SIZE、进程早期退出）
│   │       │   └─ rank-id 碰撞（两进程同一 rank）→ rank-group-mismatch
│   │       ├─ 网络连通性 → hccn_tool -link/-net_health
│   │       │   └─ DOWN/Failed → 链路故障（communication-timeout, anomaly_module=链路）
│   │       └─ HCCL_IF_IP/HCCL_SOCKET_IFNAME 配置 → 通信参数修复
│   │
│   └─ 算子执行阶段（collective 调用后 / plog 含 CommTimeout / EI0002）
│       └─ 执行超时 → 检查 collective 到齐性：
│           ├─ 各 rank 的调用编排一致？（调用次数 / 同组 collective 顺序）
│           │   ├─ 否（某 rank 多轮/少轮迭代、或同组算子顺序相反，各自序列自洽但相互错位）
│           │   │   └─ communication-timeout（编排不一致型，集合点调用错配等待超时）
│           ├─ 所有 rank 都到达 collective 调用点了吗？
│           │   ├─ 否（某 rank 跳过/sleep/死循环/提前退出）→ single-rank-block
│           │   │   └─ 定位阻塞 rank 的卡点代码（见 rank-localization.md 步骤4）
│           │   │       注：该 rank 若曾以错配 comm 句柄发起调用且入口即时失败（HCCL_E_PTR），
│           │   │       视为"已到达"，走下一分支查 comm 参数一致性
│           │   └─ 是（所有 rank 都下了 collective，含入口即失败的调用）→ 检查参数一致性：
│           │       ├─ 各 rank 调用所用 comm 句柄属同一通信域？（源码 comm 实参 + plog 各域 init 条目）
│           │       │   ├─ 否（一 rank 共享域、另一 rank solo 域/init 失败 nullptr 句柄；
│           │       │   │   错配侧 `ptr [comm] is nullptr`/HCCL_E_PTR、`comm name already exists` 即时失败）
│           │       │   │   └─ collective-param-mismatch（comm 句柄跨域不一致型）
│           │       │   │       错配域 init 成功且 collective 在错域自洽完成（no-op 正常返回）→ single-rank-block
│           │       │   └─ 是 → count/dataType/op/root 各 rank 一致？
│           │       │       ├─ 分歧/非法值集中含单侧数值退化值（count=0/空集合/空指针 buf
│           │       │       │   ——值无有效语义；该 rank 被入口拒绝即时报错退出或 no-op 返回）
│           │       │       │   └─ single-rank-block（调用退化型：到达但未构成有效参与）
│           │       │       ├─ 目标/寻址类参数越界/错配（值有语义、指向错误或不存在对象，按载体分派）
│           │       │       │   ├─ 集合调用点标量（root/srcRank 越界或互异）→ collective-param-mismatch（配对/寻址错配型）
│           │       │       │   ├─ P2P 调用点标量（HcclSend/HcclRecv dstRank/srcRank 越界）→ comm-operator-anomaly（寻址域违反）
│           │       │       │   └─ 表驱动映射构造（批次 item 表/对端基/peer 表错配）→ rank-group-mismatch（映射表错配型）
│           │       │       ├─ 分块类参数（AlltoAll per-destination 块大小）分歧
│           │       │       │   → 先按单 rank 视角核对算子语义约束（各目标分块之和 =
│           │       │       │     全局输入张量第 0 维〔各 rank 一致的业务张量元素总数，
│           │       │       │     非各 rank 自行截断分配的 sendBuf 大小〕？缓冲区 ≥ 分块×rankSize？）
│           │       │       │   ——任一 rank 单侧违反
│           │       │       │   └─ comm-operator-anomaly（算子契约违反）
│           │       │       │     各 rank 均自洽、仅块粒度互异 → collective-param-mismatch
│           │       │       ├─ 否（各值均合法互异，协议层失配暴露）
│           │       │       │   └─ collective-param-mismatch
│           │       │       │       plog "comm config of" / "CMD information check fail" 确认
│           │       │       └─ 是 → 检查链路/硬件：
│           │       │           ├─ hccn_tool 链路 DOWN/丢包 → 链路故障
│           │       │           ├─ msnpureport bbox 硬件错误 → 硬件故障
│           │       │           └─ msprof 某 rank wait 阶段异常长 → 该 rank 为长等待方（waiting rank），跨 rank 对齐同序号进入与前序计算完成时间定位晚到/偏斜对端
│           └─ 返回码是 HCCL_E_PARA(1)？
│               ├─ count 远超 buffer 大小（单 rank 即触发）→ comm-operator-anomaly
│               ├─ 参数枚举非法（非法 reduceOp 枚举等）→ comm-operator-anomaly
│               ├─ 目标/寻址类参数越界/错配（按 B1 载体分派）：
│               │   ├─ 集合调用点标量（root/srcRank 越界或互异）→ collective-param-mismatch（配对/寻址错配型）
│               │   ├─ P2P 调用点标量（HcclSend/HcclRecv dstRank/srcRank 越界）→ comm-operator-anomaly（寻址域违反）
│               │   └─ 表驱动映射构造（批次 item 表/对端基/peer 表错配）→ rank-group-mismatch（映射表错配型）
│               ├─ use-after-destroy（销毁后继续调用失效句柄）且全 rank 对称（无 rank 条件、
│               │   全部调用失败、无对端等待结构）→ comm-operator-anomaly
│               │   单 rank 条件分支过早销毁致缺席、对端等待超时 → communication-timeout
│               └─ 单 rank 传零元素/空值退化参数被入口检查拒绝，其余 rank 正常参与并等待超时
│                   → single-rank-block（调用退化型，见 B1 值类参数核对次序）
```

## 树 B：stream/event 等待环（runtime 层）

```
无报错 hang，host 线程停在 aclrtSynchronizeStream/Event
│
├─ 单进程单 device 双 stream 循环 Wait 即可复现 hang？
│   ├─ 是 → 纯 runtime 层 stream/event 等待环 → stream-event-cycle
│   │   └─ 按 (stream, op_seq) 与 event 实例/代次画操作级依赖图找环：
│   │       ├─ stream A 先 Wait(E1) 再 Record(E2) + stream B 先 Wait(E2) 再 Record(E1)（交叉互等对方后置 Record）？
│   │       │   └─ 双向循环等待环 → 拆解为操作级无环 DAG（保证各边 Record 先于对应 Wait 可达；分阶段生产→通信→回消费用独立 event 顺序成链）
│   │       ├─ event 复用混乱（同一 event 多处 Record 未配对 Reset）？
│   │       │   └─ event 复用成环 → 一 Record 一 Wait 一 Reset 成对使用
│   │       └─ Record/Wait 次序错配（Wait 先于 Record 且 Record 方被阻塞）？
│   │           └─ 次序错配环 → 调整 Record/Wait 顺序
│   └─ 否（需多 rank 才显现）→ 转树 A
│
├─ 先核查 B2 适用前提：event/stream 错用仅在个别 rank（rank 条件分支）且该 rank 因此
│   缺席后续 collective、其余 rank 通信等待超时？
│   ├─ 是，且为单纯同步原语错用（对从未 Record 的 event 调 Wait，无环/无 Reset 操作）
│   │   → single-rank-block（同步原语错用型 / P2P 配对阻塞型，不进 B2 分型）
│   ├─ 是，但为依赖图成环（双向/多 stream 循环 Wait，缺席是环的下游效应）
│   │   → 继续本树分型（等待环型 stream-event-cycle）
│   └─ 否（全 rank 对称错用 / 无缺席结构）→ 继续本树分型
│
├─ 是计算流与通信流的 event 同步问题？
│   ├─ 误用 aclrtSynchronizeStream 代替 event（host 强同步 + broken event → host 永久阻塞）
│   │   └─ sync-imbalance（类型B 错等/过度串行化；勿因 broken event 改判 cycle）
│   ├─ 单轮内 Record 时机过早（完成标记在生产行为之前 → Wait 立即通过读未就绪数据，结果错误）
│   │   └─ sync-imbalance（时序错位/标记过早型；跨轮复用未 Reset 才是 cycle 状态残留型）
│   ├─ 跨 stream 边界完全未建立 Record+Wait 依赖边（漏建）→ 数据竞争/结果错误（非 hang）
│   │   └─ stream-event-cycle（依赖缺失型）
│   ├─ 误用 aclrtSynchronizeStream 代替 event（过度串行化）→ 性能坍塌
│   │   └─ sync-imbalance（类型B 错等/过度串行化）
│   └─ event prime+reset 破坏跨 stream 可见性 → 通信流永久阻塞
│       ├─ 先判 host 同步编排：collective 前后以 SynchronizeStream 衔接计算流/通信流
│       │   （host 强同步替代跨 stream event 依赖）→ sync-imbalance（类型B 错等，
│       │     broken event 为伴生）
│       ├─ Wait 目标 event 在 Reset 后存在可达的再 Record（host 已提交、不受本等待阻塞）
│       │   └─ sync-imbalance（event 生命周期陷阱——依赖边配置完整，仅可见性被破坏）
│       └─ Reset 后无再 Record / 再 Record 不可达（排在被本 Wait 阻塞的段之后或成环）
│           └─ stream-event-cycle（次序错配型——Wait 等待永不会被 Record 的 event）
```

## 树 C：数据正确性异常分流

```
进程正常退出，但结果错乱/偶发数值异常
│
├─ 所有 rank 结果都错？
│   ├─ 在对应规模与设备集合下运行 hccl_test -c 1 是否复现 correct: no（且已排除 MPI/环境映射问题）？
│   │   ├─ 是 → 通信链路/底层环境高度可疑 → 结合 hccn_tool 丢包 + 故障窗口 msnpureport bbox + msaicerr -e 核实
│   │   └─ 否 → 所测基准窗口通过；若故障时段日志有链路告警仍须核查 hccn_tool，同时转向业务/算子侧检查：
│   │       ├─ 算子为自研通信算子（host 或 kernel 编排引擎）且无任何 API 报错？
│   │       │   损坏形态 = 远端来源段整段不符/零值、前段精确后段保持预置值、
│   │       │   双侧输出逐字节相同而期望互异（错值为合法原值——收发分段倒置）、
│   │       │   双侧输出相同而期望各不同（错值为聚合"和"——拉模式归约读切片归属错）、
│   │       │   或偶发全零+op 返回耗时异常短（间歇竞态）？
│   │       │   ├─ 是 → comm-engine-protocol-fault（配方二：写读落点/缓冲口径/
│   │       │   │   读长分段口径/切片归属/完成回发时序，按协议要素核查 ②③⑧⑨）
│   │       │   └─ 否 ↓
│   │       ├─ count/dataType/op/root 各 rank 一致？否 → collective-param-mismatch
│   │       ├─ count 远超 buffer？是 → comm-operator-anomaly
│   │       ├─ 跨 stream 边界完全未建 Record+Wait 依赖边或复用未 Reset？是 → stream-event-cycle（依赖缺失型/状态残留型）
│   │       └─ 单轮内依赖边配对齐全但 Record 时机过早？是 → sync-imbalance（时序错位/标记过早型）
│   └─ 仅偶发错（时序相关）
│       └─ 检查计算流↔通信流 event 依赖（每个 producer/consumer 边界是否有 Record+Wait）
│           ├─ 无任何 Record+Wait 依赖边 / 跨轮复用未 Reset → stream-event-cycle（依赖缺失型/状态残留型）
│           └─ 依赖边配对齐全但单轮内 Record 时机过早 → sync-imbalance（时序错位/标记过早型）
│
├─ aclnn 算子调用失败？
│   ├─ 先核对失败阶段、详细错误、最早失败 rank；对端超时可能是后果
│   ├─ 161001 → 必需输入/输出/属性空指针，查点名实参
│   ├─ 561002 → tiling 内部错误；日志指向入参约束时查 shape/属性，否则查具体内部失败
│   ├─ 561003 → kernel 查找失败，查包/路径/SoC
│   └─ 561000 → 通用内部错误，依据详细日志继续收窄，不能仅凭号码定根因
```

## 树 D：复合异常判定

先尝试用最早明确异常解释后续等待与数据差异；某rank报错、另一rank超时可以属于同一条因果链。多个日志关键词或多个rank分支本身不足以证明复合异常。

修复一个已确认缺陷后，若仍有异常，先确认改动是否真正恢复了该层语义，再检查剩余证据是否需要另一个独立缺陷解释。沿相关调用和数据流检查参数来源、内存范围与跨轮状态；正常角色分工、不同分片长度和源码注释不是故障证据。

记录每个候选根因的证据、影响和修复后的剩余现象。存在两个可分别证实、不能互相解释的缺陷时再判composite；合并修复后仍需整体编译、运行和数值验证。详见[复合异常](../symptoms/composite.md)和[落码与自验](../fix/common-constraints.md)。

## 层级判别速查表

| 判别维度 | communication-timeout | single-rank-block | stream-event-cycle | sync-imbalance |
|---------|----------------------|-------------------|-------------------|----------------|
| 故障层 | HCCL 层 | 进程层 | runtime 层 | runtime 层 |
| 是否有报错 | 有（wait timeout） | 常有（其余 rank 超时） | 无（静默 hang；依赖缺失/状态残留型为结果错误） | 无（单向阻塞/时序错位结果错误/性能差） |
| 单进程可复现 | 否（需多 rank） | 否（需多 rank） | **是**（双 stream） | 是（双 stream） |
| host 线程停在哪 | HcclAllReduce 等 | 死循环/sleep/异常分支 | aclrtSynchronizeStream | 不阻塞（时序错位型）/阻塞（lifecycle/错等型） |
| plog 关键词 | CommTimeout | 某 rank 无 notify | stream/Wait/Record | event prime/reset |

| 判别维度 | collective-param-mismatch | rank-group-mismatch | comm-operator-anomaly |
|---------|--------------------------|---------------------|----------------------|
| 故障层 | HCCL 层（语义错配） | HCCL 层（通信域协商） | 算子/HCCL 层（API 契约） |
| 返回码 | HCCL_E_TIMEOUT/INTERNAL（集合 root/srcRank 越界或失效句柄可报 HCCL_E_PARA/HCCL_E_PTR） | HCCL_E_PARA/TIMEOUT | HCCL_E_PARA / segfault |
| 触发条件 | 各 rank 参数不一致（含 comm 句柄归属）或集合标量 root/srcRank 越界 | rank-id 碰撞/worldSize 错 | 单 rank count OOB / P2P 标量越界等 |
| plog 关键词 | CMD information check fail / `ptr [comm] is nullptr` / `comm name already exists` | RANK-COLLISION / comm init fail | count OOB / aclnn fail |

## 排除规则（近似类型互斥）

1. **communication-timeout vs single-rank-block**：两者都表现为"其余 rank 超时"。区别在于 missing_rank 是否有进程级阻塞（死循环/sleep/提前退出）。有 → single-rank-block；无（rank 正常但 collective 未对齐）→ communication-timeout。**编排分歧**（各 rank 调用次数/同组 collective 顺序不一致，各自序列自洽相互错位）→ communication-timeout（编排不一致型）。
2. **stream-event-cycle vs sync-imbalance**：都涉及 stream/event。判定见 INDEX B2——依赖边缺失/复用未 Reset/成环/never-recorded（含 Reset 后无再 Record 或再 Record 不可达）→ stream-event-cycle；依赖边在且 Reset 后存在**可达的再 Record**（仅可见性被 prime/多余 Reset/错流 Reset 破坏 → 单向永久阻塞）或 SynchronizeStream 过度串行化 → sync-imbalance。勿仅凭 hang 与否或有无 prime+Reset 判型。
3. **collective-param-mismatch vs comm-operator-anomaly**：前者包含 rank 间各取值各自合法但相对不一致，以及**集合调用点标量寻址实参越界或互异**（如 `HcclBroadcast` 的 `root`/`srcRank` 越界，无论入口报 `HCCL_E_PARA` 还是等待超时均按载体归前者）；后者是单侧绝对违反算子 API/缓冲契约（count OOB/值域违反/null buf/非法枚举/recvBuf<sendCount×rankSize/分块之和≠全局输入张量第 0 维，以及 **P2P 点对点调用标量 `dstRank`/`srcRank` 越界**——寻址域违反）。
4. **collective-param-mismatch vs communication-timeout**：参数不一致最终表现为 timeout，但根因不同。plog 有参数分歧证据 → collective-param-mismatch；仅超时无参数分歧 → communication-timeout。**编排分歧（次数/顺序）不是"单一集合点参数分歧"**——各自调用序列自洽、仅相互错位 → communication-timeout。
5. **collective-param-mismatch vs single-rank-block（comm 句柄错配族）**：都用"错误 comm 句柄"致其余 rank 等待，分界在错配侧 collective 调用结局——额外通信域 init 失败（`comm name already exists`）或以 nullptr/无效句柄调用，入口**即时确定性失败**（`HCCL_E_PTR`）且该 rank 错误退出 → collective-param-mismatch（comm 参数跨 rank 不一致）；错配域 init **成功**且 collective 在错域**自洽完成**（world=1 no-op 正常返回、生命周期正常收尾、无调用级报错）→ single-rank-block（到达但未有效参与）。判型次序：先查前者。
6. **collective-param-mismatch vs single-rank-block（调用退化型，值参数）**：按**参数语义类别**判（INDEX B1 值类参数核对次序），不按调用结局（入口即拒与等待超时在各类别中均可出现，无区分力）——数值退化值（count=0/空集合/空指针，值无有效语义）→ single-rank-block（调用退化型，该 rank 未构成有效参与）；目标/寻址类参数越界按载体分派：集合调用点标量（root/srcRank）→ collective-param-mismatch（配对/寻址错配型）；P2P 调用点标量（HcclSend/HcclRecv 的 dstRank/srcRank）→ comm-operator-anomaly（寻址域违反）；表驱动参与者映射构造（item 表/对端基/peer 表）→ rank-group-mismatch（映射表错配型）；各值均合法互异 → collective-param-mismatch。
