# 修复索引

按异常类型查阅典型根因、修复策略和验证方法。以公开契约、当前 API 语义和现场证据为准，示例不替代现场判断。

## 修复输出类型与执行模式

执行模式（`core` / `full`）与工程修改权限由请求范围和证据决定：默认 `core` 利用现有日志与授权源码完成定位、获授权的最小修复与验证；`full` 在此基础上增加多卡链路与跨 Rank/Stream 依赖深挖及扩展风险评估。仅诊断未授权写码时输出 Patch 草案或修改建议；获授权修复时直接落码并完成验证。

| 类型 | 适用内容 | 适用模式与权限 |
|------|----------|----------------|
| `communication-param-fix` | 通信参数、通信域或网络配置修复 | `core` / `full`（获授权即修复并验证） |
| `sync-relation-fix` | 同步关系修正（rank 参与对等、stream-event 依赖、集合参数统一） | `core` / `full`（获授权即修复并验证） |
| `code-modification` | 最小源码与核函数/算子实现修改 | `core` / `full`（获授权直接修改并验证；未授权时输出修改建议） |
| `patch_draft` | Patch 草案（unified diff） | 只读诊断或需提交 Patch 评审时按需输出 |
| `risk_assessment` | 跨卡/跨链路影响范围、回归风险与回退方案 | `full`（或高风险改动时按需补充） |

---

## 修复落码通用约束

落码前完整读取[通用约束](common-constraints.md)；无需为读取共同规则而加载下面所有类型节。

## communication-timeout

### 典型根因 → 修复策略

| 根因分支 | 修复策略（fix_type） |
|---------|---------------------|
| rank 参与不对等（主场景）：某 rank 未到达 collective 调用点 | `sync-relation-fix`：确保所有 rank 到达 collective 调用点（参与对等）。移除 driver 中 rank 条件分支，统一调用 collective |
| 编排不一致：各 rank 调用次数分歧 / 同组 collective 顺序相反 | `sync-relation-fix`：统一各 rank 的训练循环迭代数与 collective 调用顺序（同组算子排列次序一致），确保每个集合点双方调用对齐 |
| 超时阈值配置 rank 间分歧（单位换算错误/取值不一致致失败不同步） | `communication-param-fix`：统一各 rank 的 HCCL_EXEC_TIMEOUT / HcclCommConfig.hcclExecTimeOut（及业务层自定义超时参数）配置，消除单位换算错误与阈值错配 |
| 链路异常（hccn_tool DOWN/丢包） | `communication-param-fix`：HCCL_IF_IP/HCCL_SOCKET_IFNAME 一致性、网卡 PFC 优先级、RoCE 配置（HCCL_RDMA_TIMEOUT/HCCL_RDMA_RETRY_CNT） |
| 硬件异常（msnpureport/msaicerr 命中） | 硬件排查建议（非代码修复）：硬件更换/排查 ECC/HCCS |
| 业务大算子正常慢 | 按当前 CANN 版本官方文档上调 `HCCL_EXEC_TIMEOUT`（单位：秒；取值范围与对齐步长随运行模式和产品代际变化，如部分版本 Atlas A2/A3 的 AIV 模式默认即达上限 `1091s`、AI_CPU/HOST 模式默认 `1836s` 可继续上调、早期训练/推理系列存在 `68s` 步长整除对齐，均以现场 CANN 官方环境变量说明为准）；通信域粒度用 `HcclCommConfig.hcclExecTimeOut` |

### 验证方法

- **编译验证**：工程原构建入口编译通过（退出码 0）
- **运行验证**：原复现入口确认异常已消除，公开回归命令退出码 0 且日志无异常报错模式
- **精度验证**：使用项目公开验证入口或基于公开契约的独立参考，核对全部 rank 输出通过容差比对
- **复现验证**：修复后重跑，确认无超时、无 hang、结果正确

---

## collective-param-mismatch

### 典型根因 → 修复策略

| 根因分支 | 修复策略（fix_type） |
|---------|---------------------|
| op 分歧（rank0=SUM, rank1=MAX） | `sync-relation-fix`：移除 rank 条件分支，所有 rank 统一 HcclReduceOp |
| dataType 分歧（rank0=FP16, rank1=FP32） | `sync-relation-fix`：所有 rank 统一 HcclDataType |
| count 分歧（rank0=1024, rank1=512，均合法互异） | `sync-relation-fix`：所有 rank 统一 count/sendCount/recvCount |
| root 分歧（Broadcast rank0 root=0, rank1 root=1） | `sync-relation-fix`：全局唯一 root |
| P2P 配对分歧 / 集合寻址分歧（Send/Recv 合法域内 count 或对端互异致配对等待；集合调用 root/srcRank 越界） | `sync-relation-fix`：对齐配对双方的合法 count 与收发对端（P2P 标量 `dstRank`/`srcRank` 越界属 `#comm-operator-anomaly` 寻址域违反；批次 item 表错配属 `#rank-group-mismatch`） |
| comm 句柄分歧（rank0 共享域句柄、rank1 指向其它通信域的 solo 句柄或 init 失败 nullptr 句柄，错配侧 `HCCL_E_PTR` 即时失败） | `sync-relation-fix`：所有 rank 以同一通信域句柄调用 collective（删除 solo comm 创建/三元选择分支）；comm init 失败视为致命错误，禁止 nullptr 句柄继续调用 |

**关键认知**：各 rank 取值各自合法时的跨 rank 参数分歧在协议配对阶段暴露为等待超时或一致性校验失败；集合调用点标量寻址参数越界（如 `root`/`srcRank` 超出通信域范围）或失效 comm 句柄也可在入口即时返回 `HCCL_E_PARA` / `HCCL_E_PTR`。将各 rank 集合调用参数统一为通信域内合法且一致的语义后即可恢复正常。

### 验证方法

- 编译验证 + 运行验证（退出码 0）
- 精度验证：公开数值验证或独立参考核对通过（collective 成功且结果正确，如 AllReduce SUM 后 [2.0,...]）
- 复现验证：修复后无 timeout

---

## rank-group-mismatch

### 典型根因 → 修复策略

| 根因分支 | 修复策略（fix_type） |
|---------|---------------------|
| rank-id 碰撞（硬编码 hccl_rank=0） | `communication-param-fix`：hccl_rank 按环境变量 RANK 正确派生（`uint32_t hccl_rank = static_cast<uint32_t>(rank);`） |
| worldSize 不一致 | `communication-param-fix`：核对 rank_table / RANK_ID / RANK_SIZE / worldSize 一致性 |
| 多进程端口冲突（EI0019） | `communication-param-fix`：`export HCCL_NPU_SOCKET_PORT_RANGE="auto"` |
| 集群信息校验失败（EI0014：IP 重复/IPv4-IPv6 混用/TLS 不一致） | `communication-param-fix`：修正 rank_table 中 IP/TLS 配置 |

### 验证方法

- 编译验证 + 运行验证
- 精度验证：公开数值验证或独立参考核对通过（通信域初始化成功且 HcclAllReduce 结果正确）

---

## single-rank-block

### 典型根因 → 修复策略

| 根因分支 | 修复策略（fix_type） |
|---------|---------------------|
| 死循环型（while(true)） | `sync-relation-fix`：移除或限制死循环，确保 rank 推进到 collective 调用点 |
| host 长阻塞型（sleep 120s） | `sync-relation-fix`：为 sleep 设合理上限或改条件等待 |
| 提前退出型（含资源失败被吞的假成功 exit 0） | `sync-relation-fix`：退出前完成 collective 参与 + 通信资源释放（HcclCommDestroy/HcclFinalize）；资源分配/初始化失败显式报错，禁止静默吞掉假成功 |
| 同步原语错用型（ACL error 107000） | `sync-relation-fix`：补齐缺失的 aclrtRecordEvent |
| 调用退化型（单 rank 传 count=0/空集合等退化值被入口拒绝，未构成有效参与） | `sync-relation-fix`：该 rank 参数统一为与各 rank 一致的合法有效值（如 count=elem_count），禁止零元素/空值退化参数 |
| 会话/句柄错配型 | `sync-relation-fix`：统一各 rank 的 comm 句柄与 device 会话，禁止中途切换（限错配域 init 成功且 collective 在错域自洽完成 no-op 的形态；错配侧调用即时失败〔nullptr→HCCL_E_PTR/额外域 init 失败〕属 comm 参数跨域不一致，走 [collective-param-mismatch](#collective-param-mismatch) 修复模式） |

### 验证方法

- 编译验证 + 运行验证
- 复现验证：修复后阻塞 rank 正常参与 collective、其余 rank collective 正常返回、无 hang、exit 0

---

## stream-event-cycle

### 典型根因 → 修复策略

| 根因分支 | 修复策略（fix_type） |
|---------|---------------------|
| 双向循环等待环（两 stream 交叉先 `Wait` 对方后置 `Record`，操作级成环） | `sync-relation-fix`：打破操作级等待环——按阶段因果序重组为操作级无环图（DAG 化）：每条跨流依赖保证 `Record` 在上游无阻塞可达后再由下游 `Wait`；若需生产→通信→回消费两段同步，使用独立 event 实例（如 `Ec`、`Eh`）按先后操作顺序成链 |
| event 复用错误 | `sync-relation-fix`：event 对象按"一 Record 一 Wait 一 Reset"成对使用；跨轮复用前 Reset 时须带正确的双参签名（见通用约束 1） |
| Record/Wait 次序错配 | `sync-relation-fix`：按当前API的事件捕获与完成语义调整提交次序，保证每个Wait对应可达的Record；不得把一种原语的规则推广到全部event/notify。 |
| 依赖边完全缺失 / 状态残留（跨 stream 漏建 Record+Wait 或跨轮复用未 Reset） | `sync-relation-fix`：在 producer/consumer 跨 stream 边界补齐 Record+Wait 单向依赖；跨轮复用前执行双参 Reset（见通用约束 1） |

解环后继续核对结果的数据来源、完整业务和全部输出；按[落码与自验](common-constraints.md)保留有效检查，不因存在闲置通信域就猜测缺失算法，也不把正常rank分工改成完全相同。

### 验证方法

- 编译验证 + 运行验证
- 精度验证：单向依赖下 aclrtSynchronizeStream 正常返回、两条 stream 同步成功、无 hang、exit 0
- 复现验证：双流同步无死锁阻塞且下游输出数值核对通过

---

## sync-imbalance

### 典型根因 → 修复策略

| 根因分支 | 修复策略（fix_type） |
|---------|---------------------|
| 单轮内时序错位/标记过早（依赖边配对齐全，但完成标记在生产行为之前 Record） | `sync-relation-fix`：将 `aclrtRecordEvent` 移至全部生产任务提交之后（确保下游 Wait 读到已就绪数据；若边界完全未建依赖边或复用未 Reset 属 `#stream-event-cycle`） |
| 错等/过度串行化（性能坍塌） | `sync-relation-fix`：用 event 依赖替代 aclrtSynchronizeStream，恢复计算/通信 overlap |
| event prime+reset 破坏可见性 | `sync-relation-fix`：移除 Record+Sync+Reset 预处理，改为先 Record(calcDoneEvent) 再 Wait |

**标准 event 依赖模板**（producer-consumer）：
```
计算Record(Ec) → 通信Wait(Ec) → HcclAllReduce → 通信Record(Eh) → 计算Wait(Eh)
依赖方向：computeStream(producer) → commStream(consumer) → computeStream(consumer)，单向链，无环
```

这段模板以计算完成通知通信、通信完成通知下游为目的。落码时核对当前SDK的Record/Wait语义和实际提交次序，确认下游消费的是已经完成的结果缓冲。

### 验证方法

- 编译验证 + 运行验证
- 精度验证：正确 Record-before-Wait 下 stream 同步成功、AllReduce 结果正确、公开数值验证通过

---

## comm-operator-anomaly

### 典型根因 → 修复策略

| 根因分支 | 修复策略（fix_type） |
|---------|---------------------|
| count OOB（count 远超 buffer） | `code-modification`：count 与 malloc 大小匹配（count = elem_count，buffer 同步分配 elem_count*sizeof(dtype)） |
| 值域违反（参数为溢出回绕/近类型极值/超出 API 或缓冲契约域的非法值——单 rank 视角即不可能合法；含单 rank 条件注入形态） | `code-modification`：在参数派生源（表常量/换算式/算子链路计算）恢复契约合法值——修复语义 = 单侧契约值恢复（非恢复参与、非跨 rank 对齐），勿按"统一参数"或"恢复参与"口径落码 |
| 寻址域违反（P2P 调用点 dstRank/srcRank 越界指向不存在对端） | `code-modification`：目标指向当前通信域内真实存在的对端（恢复契约配对）；若错配位于表驱动映射构造（批次 item 表/对端基）→ 按 [rank-group-mismatch](#rank-group-mismatch) 映射表错配型修复（映射随当前域规模重建） |
| 输出缓冲区不足（集合算子 recvBuf 漏乘 rankSize → 越界写/输出截断） | `code-modification`：recvBuf 按 sendCount×rankSize 分配（AllGather 类输出须容纳全部 rank 数据） |
| 分块粒度违反算子语义约束 | `code-modification`：根据公开算子与输入布局推导分块、收发范围和容量；只统一API要求一致的字段，保留合法的不均匀分片。 |
| 必需参数为空（161001） | 按详细错误恢复有效输入、输出或属性描述符；未消费的输出也可能是接口必需参数 |
| tiling 失败（561002） | 日志指出入参约束时修正 shape/属性组合并核对各 rank；否则定位具体 tiling 内部失败，不先认定用户参数非法 |
| kernel 查找失败（561003，或明确加载错误日志） | 核对实际算子包与 SoC 配置；环境变更须在已授权范围内 |
| kernel 空壳（须有证据） | `code-modification`：补齐有效 AscendC API（DataCopy/Compute/Pipe），确保生成有效 device 二进制 |
| tiling/SetBlockDim 异常 | `code-modification`：修正 tiling 函数维度/参数设置 |

### 验证方法

- 编译验证 + 运行验证
- 精度验证：公开数值验证或独立参考核对通过
- 设备自检补充观测：授权空闲设备上运行 msaicerr -e 返回 MS_AICERR_NONE_ERROR，记录为所测样例与窗口通过的侧证；仍以原运行阶段 bbox/device 日志及功能精度核对作为结案依据

---

## comm-engine-protocol-fault

### 典型根因 → 修复策略

修复语义 = **协议对称性/一致性恢复**：用户自研通信算子引擎协议层的要素错（非 API 契约违反、非库内故障），修复落点通常仅缺陷要素所在源文件一处（host 编排引擎 = 算子 op_host 源文件；kernel 编排引擎 = kernel 侧数据面编排源文件），最小修改、不改资源创建与下发编排。

| 根因分支 | 修复策略（fix_type） |
|---------|---------------------|
| 通知发布义务缺口（发布步骤被 rank 条件分支/循环边界条件化遗漏，部分 (rank, 通道) 组合未发布就绪通知） | `sync-relation-fix`：恢复**无条件全覆盖发布**——删除条件化守卫，使每个 rank 对每个远端通道都履行发布义务（发布/等待义务在 rank 维度对称） |
| 事件编号取值表两侧不一致（发布侧与等待侧沿用不同口径/陈旧口径，互不命中） | `sync-relation-fix`：两侧统一到**同一取值表**（同一事件编号口径；以现行协议语义侧为基准对齐另一侧） |
| 单边写目的槽位映射错（写入落点按陈旧映射计算，与接收方拼装取点异映射） | `code-modification`：写目的槽位与拼装读点统一为**同一映射的两侧**（本 rank 写入槽位 = 对端拼装读取槽位） |
| 拼装读源缓冲口径混用（"通道对端窗口"查询与"本域通信缓冲"查询两缓冲混用，从永不被写入的区域读数） | `code-modification`：拼装读源改取**本域通信域缓冲**（对端单边写入的实际落点域）；工程内取得而未使用的缓冲指针（死变量）消除 |
| kernel 内通知等待索引与对端发布索引异表（等待侧误用自身已发布的索引，所等事件永不被对端发布 → kernel 内有界等待超时且不回发完成通知 → host 完成等待超时） | `sync-relation-fix`：等待索引对齐**对端的发布索引**（同一取值表的配对项） |
| kernel 内软同步 flag 等待槽位错位（AIV/Vector Core 载体：等待方轮询槽位误用自视角索引——自身 rank 号槽/自身已发布槽，与发布方发布槽位两侧异表，所等 flag 永不被对端置位 → kernel 内死自旋，由 launch 级超时兜底截断 → kernel 下发后流同步超时失败） | `sync-relation-fix`：等待轮询槽位对齐**发布方槽位**（软同步取值表两侧同表：发布方写自槽、等待方轮询发布方槽） |
| kernel 内单边读长度/分段偏移口径错（读长误用固定类型尺寸口径致前段精确后段预置值；对称交换分段偏移误用自视角索引致双侧输出互补错位） | `code-modification`：读长 = 元素数 × **算子声明数据类型**尺寸；分段偏移按**目标 rank 视角**取（发送段/保留段角色随视角互换） |
| 拉模式归约读切片归属错位（从对端信箱拉取的切片索引误按对端视角取对端所属切片——拉取源切片映射与切片归属映射两侧异表 → 每 rank 归并错切片贡献，两 rank 输出同值而期望互异、错值为聚合"和"；host/kernel 载体均可） | `code-modification`：拉取源切片索引按**本 rank 归属视角**取（"我要的切片在对端数据中的位置"由本端所属切片决定，与对端是谁无关；拉取源映射与归属映射两侧同表） |
| kernel 完成回发时序提前（数据面任务未全部落地即通知 host，host 回读与设备侧任务竞态 → 间歇性输出预置值/部分拼装） | `sync-relation-fix`：完成回发调用**移至全部数据面任务落地之后**（写/读/拼装全部提交完成后） |
| kernel 数据面任务提交句柄类别错用（数据面任务误用控制通知线程句柄提交，任务不入队 → kernel 不回发完成通知，host 完成等待对称超时） | `code-modification`：数据面任务提交句柄对齐**本引擎自有线程句柄**（控制通知句柄仅承载 host↔kernel 握手） |
| kernel 成功路径引擎状态收口缺失（按标签存续的批量/会话状态 Start/End 不成对，错误路径收口而成功路径遗漏 → 同标签后续调用重入被拒，跨调用浮出） | `sync-relation-fix`：成功路径补回**状态收口调用**（与错误路径成对；置于完成回发之后） |
| kernel 通知原语类别错用（数据面就绪发布误走线程通知，对端在通道上等不到 + 本发通知被 host 完成等待错误消费 → 对端超时 + 本端假成功） | `sync-relation-fix`：就绪发布对齐**通道通知原语**（事件编号按数据面取值表；线程通知仅用于 host↔kernel 控制握手） |

### 验证方法

- 编译验证 + 运行验证（停滞配方：无编排层超时截断、全 rank 正常完成；损坏配方：数据级校验通过）
- 精度验证：使用公开验证入口或基于公开输入的独立参考检查全部rank输出。零值区间仅是定位线索，需与真实输入及预期结果对照。
- 复核协议对称：修复后逐 (rank, 通道) 核对发布/等待配对与写/读落点配对两侧一致（一次修复不得引入新的单侧口径）
- kernel 载体静态复核（无真机时）：修复 diff 须仅涉缺陷要素一处（索引/口径/时序/句柄/收口/原语类别之一），资源创建与 kernel 下发编排零改动；按协议要素核查 ①–⑨ 逐项复查该要素两侧同表

---

## composite

### 典型根因 → 修复策略

**组合规则**：不同根因 + 可独立修复 → 修复取各单一修复的**并集**。并集落码同样受「修复落码通用约束」约束——多处分歧各自统一时，统一值须在同一现场资源约束下同时可行（约束 5），且并集修复后整体核对输出工件契约（约束 6）与业务完整性（约束 4）。

复合修复沿实际失败调用或错误数据追踪关联文件。修好阻塞后继续检验结果；参数、状态和内存范围是否正确由公开语义决定，不凭rank分支或文件位置认定根因。

| 常见组合 | 修复策略 |
|---------|---------|
| stream-event-cycle + collective-param-mismatch | 拆解 stream/event 环为单向 DAG + 统一 HcclAllReduce dataType |
| single-rank-block + sync-imbalance | 移除 rank 死循环/提前退出 + 修正 event 同步编排 |

### 验证方法

- 编译验证 + 运行验证
- 精度验证：公开数值验证或独立参考核对通过（全部根因修复后）
- **逐层验证**：修复一个根因后重跑，确认是否还有残留异常（确保潜伏根因也被解决）
- **跨文件核查**：从已定位的调用反查参数来源、布局与生命周期；核对通信两端的契约匹配，保留正常的rank分工。

---

## 验证实施方法

### 端到端数据级修复验证（推荐范式）

修复成功判定（同时满足）：
1. 工程公开验证或回归命令退出码为 0（各 rank 进程正常完成）
2. 标准输出与运行日志不再出现原异常报错模式
3. 使用项目既有验证入口或基于公开数学契约的独立参考，对全部 rank、轮次与输出张量完成容差比对通过

### 通信算子特化（legacy 降级策略）

缺少数值参考时，可记录以下有限验证，但不能宣称端到端数值修复成功：
- 错误码消失且设备任务完成。
- 等待在约定时间内结束。
存在公开数学契约或独立比较入口时，仍须核对全部要求输出；不得用上述检查替代精度验收。

### 无设备回归（UT/仿真/CPU stub）

- gtest CPU stub：EXPECT_EQ 比对，毫秒级反馈
- CMAKE_ASC_RUN_MODE=cpu：`./exec` → "[Success]"/"[Failed]"
- CMAKE_ASC_RUN_MODE=sim：仿真器，可观 cycle/流水线
