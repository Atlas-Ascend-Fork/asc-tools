# collective-param-mismatch（collective 参数不一致）

## 典型现象

**HCCL 层语义错配**：各 rank 调用 collective 算子时，count/dataType/op/root 及 **comm 句柄（通信域归属）**等参数在通信域内不一致，破坏 collective 语义。

可观测信号：
- 返回码：`HCCL_E_TIMEOUT`(9) 或 `HCCL_E_INTERNAL`(4)（各 rank 取值均合法时的跨 rank 分歧在协议层暴露为卡间失步超时）；若属于集合调用点标量寻址参数越界（如 `root`/`srcRank` 指向不存在 rank）或失效句柄，错配侧也可在入口即时返回 `HCCL_E_PARA`(1) / `HCCL_E_PTR`
- plog：`[InitChannelStage][ParameterConflict]`、`CMD information {tag/cmdType/op} check fail. local[X], remote[Y]`（EI0005）
- plog：`comm config of` 字段各 rank 间不一致（count/dataType/op 分歧）
- plog：`ptr [comm] is nullptr, return HCCL_E_PTR`——某 rank 以额外通信域 init 失败后的 nullptr 无效句柄调用 collective，入口即失败；配套证据 `The comm name already exists in Group2Comm map`（额外域 init 因复用 root_info/同名失败）
- 源码：各 rank 传给同一 collective 调用的 comm 实参不同域（如 `(rank==0)?comm:solo_comm` 三元选择，一 rank 共享域 world=N、另一 rank solo 域 world=1）
- 现象：部分 rank 永久 Wait → task timeout（`TS_ERROR_AICPU_TIMEOUT`/`TS_ERROR_TASK_TIMEOUT`）

**关键约束**（各算子必须全局一致的参数）：
| 算子 | 必须一致的参数 |
|------|--------------|
| HcclAllReduce | count、dataType、op |
| HcclReduce | count、dataType、op |
| HcclBroadcast | count、dataType、root（全局唯一） |
| HcclAllGather | sendCount、dataType |
| HcclReduceScatter | recvCount、dataType、op |
| HcclAlltoAll | sendCount、sendType、recvCount、recvType |

**所有算子共通**：comm 句柄须指向**同一通信域**（各 rank 一致）——某 rank 以指向其它通信域（worldSize/成员不一致的 solo 域）或 init 失败后的 nullptr 无效句柄发起同一 collective，即 comm 参数跨 rank 不一致，属本类。

**关键特性**：各 rank 本地参数均合法时，跨 rank 参数分歧通常在协议配对或一致性检查阶段暴露为等待超时或一致性校验失败；集合调用点标量 `root`/`srcRank` 越界或失效 `comm` 句柄可在入口返回 `HCCL_E_PARA` / `HCCL_E_PTR`，仍按 B1 归 `collective-param-mismatch`。

## 调用工具

1. **[plog](../tools/plog.md)**（首选）：grep `comm config of|CMD information.*check fail|ParameterConflict`，对比各 rank 参数
2. **[plog](../tools/plog.md)** 辅助：开 `HCCL_ENTRY_LOG_ENABLE=1` 复现，打印各 rank 算子下发入参比对
3. **[msprof](../tools/msprof.md)**：确认是否因参数分歧导致某 rank 长等待

## 决策逻辑

详见 [decision-trees.md](../method/decision-trees.md) 树 A。核心判定：

1. 所有 rank 都到达 collective 调用点了吗？
   - 是（排除 single-rank-block；注意：某 rank 调用入口即失败〔如 `HCCL_E_PTR`〕也算"到达"——它已发起调用，只是 comm 参数无效）
2. plog 各 rank 的 `comm config of` / 算子入参一致？
   - count 分歧（rank0=1024, rank1=512，**两值均合法**）→ 本类
   - op 分歧（rank0=SUM, rank1=MAX）→ 本类
   - dataType 分歧（rank0=FP16, rank1=FP32）→ 本类
   - root 分歧（Broadcast rank0 root=0, rank1 root=1）→ 本类
   - 集合调用 root/srcRank 等语义错配，或 P2P 合法范围内的配对计数/合法对端互异（协议层配对失败致双侧等待）→ 本类（配对错配型）
   - comm 句柄分歧（rank0 共享域句柄、rank1 指向其它通信域的 solo 句柄/init 失败 nullptr 句柄，错配侧 `HCCL_E_PTR`/`comm name already exists` 即时失败）→ 本类（comm 句柄跨域不一致型）
   - **单侧数值退化值**（某 rank 传 count=0 零元素/空集合/空指针 buf——值本身无有效语义，被入口参数检查拒绝即时报错退出或被视作 no-op 返回，其余 rank 值合法且等待超时）→ **不改判本类**——该 rank 调用退化、未构成有效参与，按 INDEX B1「值类参数核对次序」第 1 条改道 [single-rank-block](single-rank-block.md)（调用退化型）
   - **目标/寻址类参数越界按载体分派**（INDEX B1「值类参数核对次序」第 3 条）：**集合调用点标量寻址实参**（各 rank 传给同一集合调用的 root/srcRank 越界或互异）→ 本类（配对/寻址错配型），无论入口即拒（`HCCL_E_PARA`）还是等待超时；**P2P 点对点调用标量目标参数**（`HcclSend`/`HcclRecv` 的 `dstRank`/`srcRank` 越界指向不存在对端）→ [comm-operator-anomaly](comm-operator-anomaly.md)（寻址域违反）；**表驱动映射构造条目**（批次 item 表/对端基/peer 表）→ [rank-group-mismatch](rank-group-mismatch.md)（映射表错配型）
   - **分块类参数**（如 AlltoAll per-destination 块大小）分歧时两步判（INDEX B1 值类参数核对次序第 4 条）：先按单 rank 视角核对该 rank 算子语义约束——**各目标分块之和 = 全局输入张量第 0 维（各 rank 一致的业务张量元素总数，如 elem_count；不是各 rank 自行按分块截断分配的 sendBuf 大小）**、缓冲区 ≥ 分块×rankSize——任一 rank 单侧违反 → [comm-operator-anomaly](comm-operator-anomaly.md)；各 rank 均自洽、仅块粒度互异 → 本类
   - **复合核查**：判本类（单一主型）前扫描 driver 源码全部独立注入点——存在 ≥2 个不同层注入点（算子契约/集合参数/编排/event 生命周期）且单一根因无法解释全部证据 → [composite](composite.md)

**排除近似类型**：
- 与 [communication-timeout](communication-timeout.md)：本类最终也表现为 timeout，但 plog 有参数分歧证据；communication-timeout 是参与不对等（rank 缺席/编排分歧）而非参数分歧。**编排分歧**（各 rank 调用次数/同组 collective 顺序不一致，各自调用序列本身自洽）不属本类"单一集合点参数分歧"——按 INDEX B1「编排分歧」行判 communication-timeout（编排不一致型）。本类的分歧限定在**同一集合点**上各 rank 所传算子/参数取值不同
- 与 [comm-operator-anomaly](comm-operator-anomaly.md)：本类主体是各 rank 所传值在其自身视角均合法但组内**相对不一致**，并依 B1 载体分派涵盖**集合调用点标量寻址参数越界或互异**（`root`/`srcRank` 指向不存在对端，即使错配侧在入口返回 `HCCL_E_PARA` 仍归本类）；后者则是单侧**绝对**违反算子 API 或缓冲契约——或共同违反（count OOB/值域违反〔溢出回绕/近类型极值/超缓冲域〕/非法枚举），或单 rank 违反算子语义/缓冲区约束（recvBuf 小于 sendCount×rankSize、AlltoAll 各目标分块之和与全局输入张量第 0 维不符），或 **P2P 点对点调用点标量目标参数越界**（`HcclSend`/`HcclRecv` 的 `dstRank`/`srcRank` 寻址域违反）。**值域违反不因单 rank 条件注入形态改判本类或 single-rank-block**（见 INDEX B1「值类参数核对次序」第 2、3 条）
- 与 [rank-group-mismatch](rank-group-mismatch.md)：错配目标位于**表驱动参与者映射构造**（批次 item 表/对端基/peer 映射表条目——跨 rank 配对映射的结构性数据，对端无对应视角项）→ rank-group-mismatch（映射表错配型）；位于**集合调用点标量寻址实参**（root/srcRank 等）→ 本类（配对错配型）——按 INDEX B1「值类参数核对次序」第 3 条载体分派
- 与 [single-rank-block](single-rank-block.md)（会话/句柄错配型，comm 句柄错配族分界）：本类错配侧 collective 调用**即时确定性失败**（额外域 init 失败〔`comm name already exists`〕后仍以 nullptr/无效句柄调用 → `HCCL_E_PTR`，该 rank 错误退出）；后者错配域 init **成功**且 collective 在错域**自洽完成**（world=1 no-op 正常返回、无调用级报错、生命周期正常收尾）——等待端视角该 rank 缺席。判定次序见 [INDEX.md](../INDEX.md) 边界规则 B1「comm 句柄错配族判定次序」
- 与 [single-rank-block](single-rank-block.md)（调用退化型，值参数单侧退化分界）：本类涵盖各 rank 传值各自合法互异（第 4 条）与**集合调用点标量寻址参数越界/错配**（第 3 条，包含入口即时返回 `HCCL_E_PARA` 或协议等待超时两类结局）；若分歧值集中含单侧**数值退化值**（`count=0`/空集合/空指针等无有效语义参数），该 rank 未构成有效参与 → single-rank-block（调用退化型）。判定次序见 [INDEX.md](../INDEX.md) B1「值类参数核对次序」（退化值判型先于寻址分派与相对不一致）

## 定位方法

引用 [rank-localization.md](../method/rank-localization.md)：
- 提取各 rank 的 `comm config of` 字段，逐项比对 count/dataType/op/root
- 开 `HCCL_ENTRY_LOG_ENABLE=1` 复现，查每个 rank 算子下发入参
- plog `CMD information ... check fail. local[X], remote[Y]` 直接给出分歧参数与两端取值

**示例**：`local[6] remote[2]` → 本端 cmdType=6(AllGather)，对端=2(AllReduce)，算子类型都不一致。

## 修复方向

→ [fix/INDEX.md#collective-param-mismatch](../fix/INDEX.md#collective-param-mismatch)
- `sync-relation-fix`：移除 rank 条件分支，所有 rank 统一 count/dataType/op/root
- comm 句柄分歧：所有 rank 以**同一通信域句柄**调用 collective（删除 solo comm 创建与三元选择分支）；comm init 失败视为致命错误（禁止 nullptr 句柄继续调用）
- 典型修复：driver 中 `HcclReduceOp op = (rank==0)?SUM:MAX` → 统一为 `SUM`；`HcclComm coll_comm = (rank==0)?comm:solo_comm` → 统一为 `comm`

## 交叉引用

- 易混淆：[communication-timeout](communication-timeout.md)（参与不对等致超时）、[comm-operator-anomaly](comm-operator-anomaly.md)（单 rank 绝对违反）
- 复合：常与 [stream-event-cycle](stream-event-cycle.md) 组合为 [composite](composite.md)
