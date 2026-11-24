# stream-event-cycle（stream/event 等待环）

## 典型现象

**runtime（aclrt）层故障**：跨 stream 的 **event 依赖使用错误**——含操作级循环等待环、依赖边缺失、event 复用状态残留、Record/Wait 次序错配。死锁 hang 是典型但非唯一表现：依赖缺失型/状态残留型表现为数据竞争、读到未就绪/旧数据或结果错误（进程可正常退出、退出码可为 0）。

可观测信号：
- 程序卡死/挂起（hang），**无报错信息**输出，进程不退出
- host 线程常阻塞在 `aclrtSynchronizeStream` / `aclrtSynchronizeDevice` / `aclrtSynchronizeEvent`
- 日志中通常**没有** HCCL 的 `wait timeout` 字样（stream 已在 event Wait 处阻塞，HCCL 超时计时器未启动）
- device 侧相关 stream 长时间空转/等待，无实际算子执行推进
- msprof stream 时间线：对应 stream 出现长空闲/等待段且不结束
- **依赖缺失型（非 hang）**：跨 stream producer/consumer 边界无任何 Record+Wait 配对 → 下游读到未写完/未就绪数据 → 数据竞争/结果错误，进程正常退出
- **状态残留型（非 hang）**：event 跨轮/跨阶段复用未 Reset，残留 signaled 状态 → Wait 提前/立即返回，读到上一轮旧数据 → 结果错误

**与通信超时的本质区别**（关键诊断分叉点）：
| 维度 | stream-event-cycle | communication-timeout |
|------|-------------------|----------------------|
| 触发层级 | runtime（aclrt）层 event 依赖死锁 | HCCL 层卡间 collective 同步等待超时 |
| 是否有报错 | 无（静默 hang） | 有（wait timeout + 错误码） |
| 是否触发 HCCL 超时 | 通常不触发（算子未到达） | 触发 HCCL_EXEC_TIMEOUT |
| 单进程可复现 | **是**（单 device 双 stream 循环 Wait 死锁） | 否（需多 rank） |

## 调用工具

1. **gdb/py-spy attach**（首选）：查看 host 线程栈是否长期停在 `aclrtSynchronizeStream`/`aclrtSynchronizeEvent`
2. **[msprof](../tools/msprof.md)**：开 Profiling（task/acl 维度），查看 stream timeline，画依赖图找环
3. **[plog](../tools/plog.md)**：grep `stream|event|Wait|Record|deadlock`，提取 stream/event 等待链特征

## 决策逻辑

详见 [decision-trees.md](../method/decision-trees.md) 树 B。核心判定：

0. **先核查 B2 适用前提**（INDEX B2）：本类的 stream/event 错用须为**全 rank 同源对称**（同一代码路径、双侧同形态）或无 rank 缺席结构（各 rank 对称卡在 runtime 同步点/数据错误）。若错用仅在个别 rank（rank 条件分支）且导致该 rank 缺席后续 collective、其余 rank 等待超时，按错用性质分流——**单纯同步原语错用**（对从未 Record 的 event 调 Wait：无环、无 Reset 操作、源码中该 event 无任何 Record）→ [single-rank-block](single-rank-block.md) 同步原语错用型；**依赖图成环**（双向/多 stream 循环 Wait，环内可含 collective，缺席是环的下游效应）→ 仍属本类（等待环型）。
1. **先判层**：日志中有 `wait timeout`/HCCL 错误码？
   - 有 → 转 [communication-timeout](communication-timeout.md)（HCCL 层）
   - 无且 hang → 优先怀疑 runtime 层死锁（本类）
   - 无报错且**结果错误/数据竞争**（进程正常退出）→ 仍归本类（依赖缺失型/状态残留型，见 [INDEX.md](../INDEX.md) 边界规则 B2）
2. **单进程可复现**：单进程单 device 双 stream 循环 Wait 即可复现 hang？
   - 是 → 纯 runtime 层（本类）
   - 否（需多 rank）→ 转其他类型
3. **按 `(stream, op_seq)` 与 event 实例/代次画操作级依赖图找环**：
   - stream A 先 `Wait(E1)` 再 `Record(E2)` + stream B 先 `Wait(E2)` 再 `Record(E1)`（双方交叉互等对端排在本阻塞点之后的 `Record`，操作级成环）→ 双向循环等待环（区别于先 `Record(Ec)` 触发对端 `Wait(Ec)`、完成后再 `Record(Eh)` 供原流后续 `Wait(Eh)` 的分阶段无环 DAG）
   - event 复用混乱（同一 event 多处 Record 未配对 Reset）→ event 复用成环
   - Record/Wait 次序错配（Wait 先于 Record 且 Record 方被阻塞；**或 event 被 Reset 后再无任何 Record / 再 Record 排在被本 Wait 阻塞的段之后而不可达**）→ 次序错配环
4. **查依赖边缺失与状态残留**（非 hang 型，[INDEX.md](../INDEX.md) 边界规则 B2）：
   - producer/consumer 跨 stream 边界无任何 Record+Wait 依赖边（漏建）→ 依赖缺失型（数据竞争/结果错误，判本类）
   - event **跨轮/跨阶段复用**未 Reset（残留 signaled → 本轮 Wait 立即/提前返回读旧数据）→ 状态残留型（结果错误，判本类）；**单轮内** Record 时机/位置错误（完成标记在生产行为之前过早 Record → Wait 立即通过读未就绪数据）→ 转 [sync-imbalance](sync-imbalance.md)（时序错位/标记过早型，非跨轮状态残留）
5. **prime/Reset + 单向 Wait 永久阻塞的重叠消解**（[INDEX.md](../INDEX.md) B2「Reset 破坏可见性与 never-recorded Wait 的重叠消解」）：核对 Wait 目标 event 在 Reset 之后**是否存在可达的再 Record**（源码序 + host 提交次序）——无再 Record 或再 Record 不可达（被本等待阻塞）→ 本类（次序错配型，Wait 等待一个永不会被 Record 的 event）；存在可达的再 Record → 转 [sync-imbalance](sync-imbalance.md)（event 生命周期陷阱，依赖边配置完整仅可见性被破坏）。**勿仅凭"有 prime+Reset"即判 sync-imbalance**

**排除近似类型**：
- 与 [communication-timeout](communication-timeout.md)：本类无报错静默 hang；communication-timeout 有 wait timeout 报错
- 与 [single-rank-block](single-rank-block.md)：本类所有 rank host 栈均停在 aclrt 同步 API，无 rank 缺席；single-rank-block 有 rank 缺席
- 与 [sync-imbalance](sync-imbalance.md)：按 [INDEX.md](../INDEX.md) 边界规则 B2 判——依赖边**完全未建立**（漏建）或复用**未 Reset**（Wait 立即返回/数据竞争/结果错误）→ 本类；依赖边已建立且 **Wait 目标 event 在 Reset 后存在可达的再 Record**，仅被 Reset 预处理/迭代间多余 Reset/错流 Reset 破坏可见性（单向 Wait **永久阻塞**）或 SynchronizeStream 过度串行化（性能坍塌）→ sync-imbalance；**Reset 后无再 Record 或再 Record 不可达（被本等待阻塞）→ 本类（次序错配型）**。**勿以"hang 与否"或"有无 prime+Reset"作唯一判据**

**依赖缺失型异常作用点报告要求**（诊断报告第 3 节）：数据竞争类（非 hang）的等待点/异常作用点须**枚举跨 stream 边界两侧的全部生产/消费操作**，逐项列出、不得遗漏：
- 写方（producer）侧**每一项**写 dev 缓冲的操作：**aclnn 自定义算子执行**（算子在 producer stream 上写入输出 tensor，如 `aclnn<Xxx>(..., producerStream)`——按源码实参列出算子名）、`aclrtMemcpy/MemcpyAsync` 数据搬移、其它 kernel 写入；
- 读方（consumer）侧的 collective/算子调用；
- **缺失的依赖边本身**（应建而未建的 Record→Wait 配对，指明 producer event 名与 consumer stream）。
只报一侧端点或只报 memcpy 漏报算子执行（反之亦然）都会导致竞争窗口描述不完整。

## 定位方法

引用 [rank-localization.md](../method/rank-localization.md) 第 6 节：
- 按 `tid`/`pid` 关联 rank → `(stream, op_seq)` 操作节点，保留同 stream 内提交先后有向边
- 按具体 event 实例或代次建立跨流依赖有向边（`Record(E_i)` 操作节点 → `Wait(E_i)` 操作节点，不将整条 stream 折叠为单节点）
- 在操作级有向图上做拓扑/DFS 检测：存在操作级循环等待环或 `Wait` 指向不可达 `Record` → 死锁根因

**等待环形成条件**：
- 条件 A（双向循环等待环）：两 stream 在同一推进阶段互相先 `Wait` 对方排在本阻塞点之后的 `Record`（区别于 `compute Record(Ec) → comm Wait(Ec) → AllReduce → comm Record(Eh) → compute Wait(Eh)` 按先后操作成链的合法无环 DAG）
- 条件 B（次序错配+链路阻塞）：Wait 先于 Record 下发，Record 方又被阻塞
- 条件 C（event 复用错误）：上一轮依赖未结束就被新 Record 覆盖；或复用前未 Reset，残留 signaled 状态使 Wait 立即返回（读到旧数据）
- 条件 D（依赖边缺失）：跨 stream producer/consumer 边界未建立任何 Record+Wait 边 → 数据竞争（非 hang）

## 修复方向

→ [fix/INDEX.md#stream-event-cycle](../fix/INDEX.md#stream-event-cycle)
- `sync-relation-fix`：打破操作级等待环——按阶段因果序重组为操作级无环图（DAG 化）
- 同一推进阶段内避免两 stream 交叉先 `Wait` 对方尚未提交的 `Record`（分阶段生产→通信→回消费须使用独立 event 实例按先后顺序成链）
- event 对象按"一 Record 一 Wait 一 Reset"成对使用

## 交叉引用

- 易混淆：[communication-timeout](communication-timeout.md)（HCCL 层，有报错）、[single-rank-block](single-rank-block.md)（进程层，有 rank 缺席）、[sync-imbalance](sync-imbalance.md)（Reset 破坏可见性/串行化失衡；边界见 INDEX 规则 B2）
- 复合：常与 [collective-param-mismatch](collective-param-mismatch.md) 组合为 [composite](composite.md)
