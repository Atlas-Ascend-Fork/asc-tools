# composite（复合异常）

## 典型现象

**多重根因组合**：存在两个或更多可分别证实的缺陷，单一根因不能解释全部证据；某个缺陷可能掩盖另一个。

可观测信号：
- 现象被某一根因主导（表象），另一根因潜伏（修复主导根因后才暴露）
- 典型组合：stream-event-cycle（环形死锁，表象为 hang）+ collective-param-mismatch（dataType 分歧，潜伏第二根因）
- 进程退出码非 0（多个异常分支至少一个触发）
- 或设备输出缺失、截断、与公开参考结果数值偏离
- 严重度：critical

**常见组合模式**（按实际用例归纳）：
| 组合 | 表象 | 潜伏根因 |
|------|------|---------|
| stream-event-cycle + collective-param-mismatch | 环形死锁 hang | dataType 分歧（HcclAllReduce 不可达） |
| single-rank-block + stream-event-cycle | rank 提前退出 + event 错流 Record | 双重独立根因 |
| collective-param-mismatch + comm-operator-anomaly | 参数分歧 + count OOB | 双重独立根因 |
| comm-operator-anomaly + stream-event-cycle | 算子契约违反/调用失败 + 发送流 Wait 不可满足 event 永久阻塞 | 双重独立根因（一侧算子层、一侧 runtime 层） |
| comm-operator-anomaly + communication-timeout | 缓冲区契约违反 + 另一 rank 迭代间长阻塞（checkpoint/IO）超 watchdog | 双重独立根因（算子层 + 参与时机） |
| collective-param-mismatch + communication-timeout | 集合参数分歧 + 各 rank 超时阈值配置分歧（如单位换算错误、阈值不一致致失败不同步） | 双重独立根因（协议层 + 配置层） |

**复合识别要点**：① 单一异常类型的修复模式无法同时覆盖两侧证据（如既有算子调用失败/越界证据、又有通信等待超时证据，且两者不能用一个根因解释）→ 逐项核对是否双根因；② rank 间**配置类分歧**（超时阈值/watchdog 单位）与**调用类分歧**（参数/编排）可各自独立成根因；③ 各分量按对应单一类型的判定规则独立鉴型，主型标 composite。

## 调用工具

1. **[plog](../tools/plog.md)**（首选）：同时提取多类特征关键词（timeout + stream/event + comm config）
2. **[msprof](../tools/msprof.md)**：stream timeline 找环 + 通信矩阵找参数分歧
3. **gdb/py-spy attach**：host 线程栈判断是死锁还是通信等待
4. 各单一类型的对应工具（见交叉引用）

## 决策逻辑

详见 [decision-trees.md](../method/decision-trees.md) 树 D。核心判定：

1. **单一根因无法完全解释现象**？
   - 先确认该根因已被实际修好；剩余异常有独立证据时，再考虑复合根因。
2. **plog 同时出现两类特征**？
   - stream Wait 成环 + comm config dataType 分歧 → composite
   - 某 rank 无 notify（single-rank-block）+ event 错流 Record（sync-imbalance）→ composite
3. **源码分支只提供检查入口**：按rank选择角色、数据范围或目标可以完全合法，不能只凭多个分支判composite。
4. **按证据检查关联代码**：从失败调用或第一处错误数据反查参数派生、查表、缓存与生命周期。只有出现不能被当前因果链解释的独立缺陷证据，才增加根因；部分区间零值也可能来自合法输入或其他数据路径，不能直接指定为参数表问题。
5. **逐层剥离策略**：
   - 先解决表象根因（如环形死锁），再看潜伏根因（如参数分歧）是否暴露
   - 每层剥离后重跑复现，确认是否还有残留异常

**组合规则**：
- 不同根因 + 可独立修复 → 修复取各单一修复的**并集**
- 各根因间无因果依赖（一个根因不是另一个根因的诱因）

## 定位方法

引用 [rank-localization.md](../method/rank-localization.md)：
- 对每个 rank 同时检查：进程生命周期（single-rank-block）、comm config 参数（collective-param-mismatch）、stream/event 依赖图（stream-event-cycle）
- msprof trace 综合分析：stream 环 + 通信矩阵参数分歧 + wait 阶段
- 源码审查：核对相关调用的参数来源和正常rank分工，记录违反公开契约的具体位置。

## 修复方向

→ [fix/INDEX.md#composite](../fix/INDEX.md#composite)
- 各已确认根因的修复需共同满足[落码与自验](../fix/common-constraints.md)，合并后验证整体语义；不机械拼接彼此冲突的补丁。
- 示例（stream-event-cycle + collective-param-mismatch）：
  - 拆解 stream/event 操作级等待环为无环 DAG（消除交叉先 Wait 后 Record 的循环等待）
  - 统一 HcclAllReduce dataType（所有 rank 一致）
- `sync-relation-fix` + `communication-param-fix`

## 交叉引用

- 组成成分：[stream-event-cycle](stream-event-cycle.md)、[collective-param-mismatch](collective-param-mismatch.md)、[single-rank-block](single-rank-block.md)、[sync-imbalance](sync-imbalance.md)、[comm-operator-anomaly](comm-operator-anomaly.md)
- 诊断策略：逐层剥离，参考各单一类型的决策树
