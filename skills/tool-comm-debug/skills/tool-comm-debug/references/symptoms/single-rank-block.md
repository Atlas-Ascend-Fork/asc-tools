# single-rank-block（单 rank 阻塞）

## 典型现象

**进程/业务逻辑层故障**：某一个 rank 因进程级生命周期问题（死循环、host 侧长阻塞、提前/异常退出、不响应 finalize、通信句柄/设备会话错配）未能正常参与 collective，导致其余 rank 在通信等待点持续等待。

可观测信号：
- 其余 rank 卡在 collective 调用（HcclAllReduce/HcclBroadcast/HcclFinalize）不返回
- 最终被 HCCL watchdog / HCCL_EXEC_TIMEOUT 终止，打印 wait timeout
- 阻塞 rank 自身可能无报错（死循环/长 sleep 静默 hang），或打印 `stuck in infinite loop, not entering collective`
- ACL error 107000（对未 Record 的 event 调 StreamWaitEvent）
- host 线程：阻塞 rank 停在 driver 的死循环/sleep/异常分支；其余 rank 停在 HcclAllReduce

**成因分类**（按实际注入归纳）：
| 类型 | 表现 |
|------|------|
| 死循环型 | rank 在 collective 前 `while(true)` 忙等，永不推进到通信调用点 |
| host 长阻塞型 | rank 在 collective 前 sleep/IO 阻塞（如 sleep 120s） |
| 提前退出型 | rank 未调用 collective，退出路径也未调用通信域销毁接口（HcclCommDestroy）即 return/exit——即使已释放本地计算资源/已做 ACL 运行时级收尾（aclrtResetDevice/aclFinalize），通信域句柄仍被遗弃交由进程退出自动回收；**资源分配/初始化失败被静默吞掉后的假成功退出（exit 0）同属本型**——该 rank 实际未参与 collective，其余 rank 等待超时 |
| 同步原语错用型 | rank 对**从未被 Record** 的 event 调 StreamWaitEvent（ACL error 107000，或运行时接受该 Wait 致 stream 永久阻塞）——特征：该 event 在源码中**无任何 Record 调用**（单纯漏配原语，无环、无 Reset 生命周期操作）；限单 rank 条件分支注入：该 rank 因错用缺席后续 collective、其余 rank 正常等待超时。**例外（不判本类）**：① 同源错用为全 rank 对称（同一代码路径）→ 进 INDEX B2 分型（stream-event-cycle / sync-imbalance）；② 依赖图成环（双向/多 stream 循环 Wait，环内可含 collective）→ stream-event-cycle 等待环型（环是根因，缺席是下游效应） |
| P2P 配对阻塞型 | rank 在集合调用**之前**的点对点调用（HcclSend/HcclRecv）上因对端无匹配调用而阻塞（配对缺失/错配），未到达集合调用点，其余 rank 在集合等待点超时——属参与问题，非"编排分歧"（编排分歧限各 rank 集合调用序列本身错位 → communication-timeout） |
| 会话/句柄错配型 | rank 中途切换 device；或用错误 comm 句柄但**错配域 init 成功且 collective 在错域自洽完成**（world=1 no-op 正常返回，rank 无调用级报错、生命周期正常收尾）。注意：错配侧通信域 init 失败/nullptr 句柄致 collective 调用**即时失败**（`HCCL_E_PTR`）并错误退出时，属各 rank comm 参数跨域不一致 → [collective-param-mismatch](collective-param-mismatch.md)（见 INDEX B1 comm 句柄错配族判定次序） |
| 调用退化型 | rank **到达调用点**但所传值参数为**数值退化值**（count=0 零元素/空集合/空指针 buf——值本身无有效语义），被调用入口参数检查拒绝即时报错退出，或被视作 no-op 正常返回——两种结局均为"未构成有效参与"，其余 rank 在通信等待点等待超时。退化参数本身是该 rank 的直接根因（修复 = 统一为合法值，恢复有效参与）。**注意**：① srcRank/dstRank/root 等目标/寻址类参数错配**不属本型**——值有语义、指向错误对象，按 INDEX B1「值类参数核对次序」第 3 条**载体分派**（集合调用点标量 → [collective-param-mismatch](collective-param-mismatch.md) 配对错配型；P2P 点对点标量 → [comm-operator-anomaly](comm-operator-anomaly.md) 寻址域违反；表驱动映射构造条目 → [rank-group-mismatch](rank-group-mismatch.md) 映射表错配型），无论该侧调用入口即拒还是等待超时，修复语义都是纠正寻址而非恢复参与；② **溢出/超界非法大值（值域违反——整型回绕/近类型极值/超出缓冲契约域）不属本型**——"以非法载荷调用"参与意图存在，非参与退化 → [comm-operator-anomaly](comm-operator-anomaly.md)（参数域契约违反型）。与 collective-param-mismatch 的分界见 INDEX B1「值类参数核对次序」 |

## 调用工具

1. **[plog](../tools/plog.md)**：grep 每个进程的 collective 调用日志，找出无 `Entry-HcclAllReduce` 的 rank
2. **gdb/py-spy attach**：查看各 rank host 线程栈，确认阻塞 rank 卡在哪段非通信代码
3. **[msprof](../tools/msprof.md)**：trace 表现——阻塞 rank 通信 stream 上无 collective 任务

## 决策逻辑

详见 [decision-trees.md](../method/decision-trees.md) 树 A + 速查表。核心判定：

1. 其余 rank 是否卡在 HcclAllReduce/HcclBroadcast/HcclFinalize？是 → 进入多 rank 参与度核查
2. 逐 rank 核对参与度：所有 rank 是否都已到达并调用 collective？
   - 找出"未到达"或"到达但未正确参与"的 rank
3. 定位阻塞 rank 的卡点代码：
   - 死循环（while(true)）/长 sleep / 异常分支（ACL error 107000）/ 已退出（进程不在）
4. 核对 collective 组一致性：comm 句柄、worldSize、device 会话是否一致

**排除近似类型**（关键分叉点）：
- 与 [stream-event-cycle](stream-event-cycle.md)：所有 rank host 栈均停在 aclrt 同步 API 且无 rank 缺席 → stream-event-cycle；某 rank 缺席 → single-rank-block
- 与 [communication-timeout](communication-timeout.md)：本类根因在**进程层**（被阻塞 rank 的代码逻辑）。当等待端同时出现 watchdog/hcclExecTimeOut 超时报错时，按 [INDEX.md](../INDEX.md) 边界规则 B1 依**本 rank 形态**判主型：host 有限时长长阻塞（sleep/IO 阻塞后才到达）/ 死循环永不推进 / 绕过通信域销毁直接退出（无通信域销毁调用证据即退出，ACL 级收尾不算；含假成功 exit 0）/ 到达但调用退化未有效参与（退化值被入口拒绝或零元素 no-op）/ 会话句柄错配（切 device/未 Record 的 event Wait/错 comm 句柄且错域 init 成功、collective 在错域自洽完成 no-op）→ 本类；对端崩溃退出（abort/core dumped）/ 跳过但**显式销毁通信域**后退出（有销毁调用证据；仅有退出期自动回收清理日志时归本类"绕过销毁直接退出"）/ collective 前过早销毁 comm/stream / 建链期 device 同卡争抢 / **编排分歧**（各 rank 调用次数/同组 collective 顺序不一致，各自序列自洽相互错位）→ communication-timeout（本类仅为表现层）；**错 comm 句柄且各 rank 通信域归属不一致、错配侧调用即时失败（nullptr→`HCCL_E_PTR`/额外域 init `comm name already exists` 失败后仍调用）→ collective-param-mismatch**（优先判，见 B1 comm 句柄错配族判定次序）
- **单进程可复现性**：等待环在单进程双 stream 即可 hang；single-rank-block 需多 rank（collective 至少 2 rank）

## 定位方法

引用 [rank-localization.md](../method/rank-localization.md)：
- 逐 rank 查 host 线程栈（gdb/py-spy attach），找出未到达 collective 的 rank
- 查该 rank 栈停在 driver 的哪段代码——死循环/sleep/异常分支/已退出
- 核对各 rank 的 comm 句柄、worldSize、root_info、device 会话一致性

## 修复方向

→ [fix/INDEX.md#single-rank-block](../fix/INDEX.md#single-rank-block)
- 死循环/长阻塞型：移除或限制阻塞逻辑（去掉 while(true)、为 sleep 设合理上限）
- 提前退出型：退出前完成 collective 参与 + 通信资源释放（HcclCommDestroy/HcclFinalize）；资源失败不得静默吞掉（分配失败应显式报错退出或对齐参与）
- 同步原语错用型：补齐缺失的 aclrtRecordEvent
- 调用退化型：将退化参数（零元素 count/空集合）改为与各 rank 一致的合法有效值（如统一 count=elem_count），禁止传 0/退化值；若该参数在各 rank 间同时存在合法取值互异，须一并统一
- 句柄/会话错配型：统一各 rank 的 comm 句柄与 device 会话；若属各 rank comm 参数跨域不一致（错配侧即时失败）按 [collective-param-mismatch](../fix/INDEX.md#collective-param-mismatch) 修复模式

## 交叉引用

- 易混淆：[communication-timeout](communication-timeout.md)（共存时主型归属见 INDEX 边界规则 B1）、[stream-event-cycle](stream-event-cycle.md)（runtime 层，单进程可复现）
- 注：本类常表现为"其余 rank wait timeout"，但根因在被阻塞 rank 的进程生命周期；判主型须核对缺席 rank 的退出/阻塞形态（是否**显式销毁通信域**——以销毁调用条目/源码收尾路径为准，进程退出期自动回收清理日志不算；是否崩溃信号；是否有限时长阻塞）
