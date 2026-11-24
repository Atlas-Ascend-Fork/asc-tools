# sync-imbalance（计算流与通信流同步失衡）

## 典型现象

**runtime 层 event 依赖问题**：通算融合/通算重叠场景下，计算流（computeStream）与通信流（commStream）之间的 event 依赖编排失衡——依赖边已按单向模板建立，但被生命周期操作破坏可见性或被串行化替代，导致单向永久阻塞或性能坍塌。

可观测信号：
- **类型 A（依赖失效/event 生命周期陷阱）**：依赖边存在、方向正确，但被生命周期操作破坏 → 单向 Wait 永久阻塞（hang）或依赖退化
  - prime 预处理（Record+Sync+Reset）后跨 stream 可见性丢失：commStream 的 `StreamWaitEvent` 永久阻塞
  - 迭代间/轮间多余 Reset：可见性丢失，下一轮 Wait 永久阻塞
  - Record/Reset 执行在错误 stream（consumer 侧而非 producer 侧）：可见性丢失
  - 危害：依赖边形存实亡，典型表现为计算流/通信流单向永久阻塞（hang）
- **类型 B（错等/过度串行化）**：性能坍塌（结果正确）或 host 阻塞
  - 误用 `aclrtSynchronizeStream(commStream)` 代替 event 依赖：Host 强同步致 overlap 失效（含 host 强同步 + 生命周期损坏的 broken event 组合 → host 永久阻塞）
  - Record/Wait 位置过粗：扩大串行区间，压缩重叠窗口
  - **单轮内 Record 时机/位置过早**（完成标记在生产行为之前 Record，如通信完成标记在 collective 调用之前）→ 下游 Wait 立即通过、读到未就绪数据（结果错误，非 hang）——时序错位/标记过早型；与 stream-event-cycle 状态残留型的分界：本型为**单轮内时序错误**（依赖边与配对齐全），后者为**跨轮/跨阶段复用**未 Reset 的状态未清理
  - 危害：性能问题或时序相关的数据正确性问题
- **event 生命周期陷阱**：`calcDoneEvent` 被 Record+Sync+Reset 预处理后，跨 stream 可见性丢失，commStream 的 `StreamWaitEvent` 永久阻塞

**边界（关键）**：若跨 stream producer/consumer 边界**完全未建立依赖边**（漏建 Record+Wait）或 **event 复用未 Reset**（残留 signaled → Wait 立即返回/数据竞争/结果错误）→ 属 [stream-event-cycle](stream-event-cycle.md)（依赖缺失型/状态残留型），**不判本类**（判据见 [INDEX.md](../INDEX.md) 边界规则 B2）。**本类（event 生命周期陷阱型）的构成前提**：依赖边已建立、方向正确，**且 Wait 目标 event 在 Reset/prime 之后存在可达的再 Record**（host 已提交、位于不受本等待影响的 stream 段）——仅初始可见性被生命周期操作破坏；若 Reset 后**再无任何 Record**、或再 Record 排在（直接或间接）被本 Wait 阻塞的段之后（如与 Wait 同 stream 且后于 Wait、或 Record 方反向依赖形成环），Wait 的满足条件永不可达 → 属 [stream-event-cycle](stream-event-cycle.md)（次序错配型），不判本类（消解判据见 INDEX B2「重叠消解」）。

## 调用工具

1. **[msprof](../tools/msprof.md)**（首选）：查看计算与通信时间线是否预期重叠；定位异常 `SynchronizeStream`/过粗 Record 位置
2. **[plog](../tools/plog.md)**：grep `stream|event|calcDoneEvent|prime|reset`，提取 event 同步编排
3. **数据 dump**：dump 通信前后 tensor，确认读写时序（时序错位/标记过早型；若完全未建依赖边属 [stream-event-cycle](stream-event-cycle.md)）

## 决策逻辑

详见 [decision-trees.md](../method/decision-trees.md) 树 B/树 C。核心判定：

1. 是 hang 还是结果错误？
   - 单向 Wait 永久阻塞（依赖边在、**Reset 后存在可达的再 Record**、event 被 prime+Reset/迭代间多余 Reset/错流 Reset 破坏可见性）→ 本类（类型 A 生命周期陷阱）
   - 数据竞争/读到未就绪或旧数据/结果错误，且**依赖边完全未建立**或 **event 复用未 Reset** → 转 [stream-event-cycle](stream-event-cycle.md)（[INDEX.md](../INDEX.md) 边界规则 B2，勿按"漏等"判本类）
   - 单向 Wait 永久阻塞但 **Reset 后无再 Record / 再 Record 不可达**（等待一个永不会被 Record 的 event）→ 转 [stream-event-cycle](stream-event-cycle.md)（次序错配型，见 INDEX B2「重叠消解」）
   - 性能差但结果正确（SynchronizeStream 串行化/过粗 Record 位置）→ 本类（类型 B 错等）
   - host 强同步（SynchronizeStream 替代 event 依赖）+ 生命周期损坏的 broken event → host 永久阻塞 → 本类（类型 B，勿因 broken event 改判 stream-event-cycle——host 同步编排是主根因，broken event 为伴生；判别见 INDEX B2「重叠消解」先判 host 同步编排）
   - 结果错误且为**单轮内 Record 时机过早**（标记先行于生产行为、依赖边配对齐全）→ 本类（时序错位/标记过早型）；跨轮复用未 Reset 的状态残留 → 转 stream-event-cycle（状态残留型）
2. 检查每个 producer/consumer 边界是否有配套 Record+Wait：
   - 标准 event 依赖模板：`计算Record(Ec) → 通信Wait(Ec) → 通信Record(Eh) → 计算Wait(Eh)`（单向链，无环）
   - 边界完全无依赖边（漏建 Record+Wait）→ 转 [stream-event-cycle](stream-event-cycle.md)（依赖缺失型）
   - 依赖边在但被 Reset/错流操作破坏 → 类型 A
3. 检查是否误用 SynchronizeStream 代替 event → 类型 B

**排除近似类型**：
- 与 [stream-event-cycle](stream-event-cycle.md)：按 [INDEX.md](../INDEX.md) 边界规则 B2 判——依赖边缺失、event 复用未 Reset（Wait 立即返回/数据竞争/结果错误）、依赖成环、never-recorded Wait（含 Reset 后无再 Record 或再 Record 不可达）→ stream-event-cycle；依赖边在、**Reset 后存在可达的再 Record**、仅 Reset 预处理/迭代间多余 Reset/错流 Reset 致单向 Wait 永久阻塞，或 SynchronizeStream 过度串行化 → 本类。**勿以"hang 与否"或"有无 prime+Reset"作唯一判据**（本类生命周期陷阱型与 stream-event-cycle 等待环型均可表现为 hang、均可含 prime+Reset；分界在 Reset 后再 Record 的可达性）
- 与 [collective-param-mismatch](collective-param-mismatch.md)：本类是单 device 内两条 stream 的 event 依赖问题（runtime 层）；collective-param-mismatch 是卡间参数不一致（HCCL 层）

**概念边界**（避免混淆）：
- commStream 内"通信算子之间"的卡间同步由 HCCL 治理（HCCL_EXEC_TIMEOUT），不属于 runtime event
- commStream 与 computeStream 之间的本地依赖才用 aclrt event（本类）

## 定位方法

- 功能错误类：dump 通信前后 tensor 比对；检查每个 producer/consumer 边界是否有 Record+Wait
- 性能坍塌类：msprof 查看计算与通信时间线重叠度；定位异常 SynchronizeStream/过粗 Record
- event 生命周期类：检查同一 event 是否被 prime+reset 预处理破坏可见性

## 修复方向

→ [fix/INDEX.md#sync-imbalance](../fix/INDEX.md#sync-imbalance)
- 生命周期陷阱型：移除 Record+Sync+Reset 预处理/迭代间多余 Reset，改为先在 producer stream 上 Record 再由 consumer Wait
- 错等型：用 event 依赖替代 `SynchronizeStream`，恢复计算/通信 overlap
- event 陷阱型：移除 Record+Sync+Reset 预处理，改为先 Record 再 Wait

## 交叉引用

- 易混淆：[stream-event-cycle](stream-event-cycle.md)（依赖缺失/状态残留/等待环；边界见 INDEX 规则 B2）、[collective-param-mismatch](collective-param-mismatch.md)（卡间参数）
- 标准 event 依赖范式见修复章节
