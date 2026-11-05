# flag_mismatch — set_flag/wait_flag 不配对

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | flag_mismatch |
| 异常类型 | kernel hangs（永久阻塞） |
| 错误码 | 无（kernel 挂起型） |
| 类别 | sync_hang |
| 源文件 | cann-aicore/03_sync/flag_mismatch_hangs/error/main.asc |
| 报错模式 | kernel_hangs |
| 内存区域 | 多区域（同步原语跨流水线级） |
| kernel_diff_type | kernel_diff |
| behavior_note | kernel挂起型，无EZ9999输出 |

## 触发条件

在昇腾 NPU 编程模型中，`SetFlag`（底层对应 `asc_sync_block_set`）用于向指定 event 发送同步信号，`WaitFlag`（底层对应 `asc_sync_block_wait`）用于等待某个 event 的信号到达。两者必须严格配对使用：每个被等待的 event 必须有且仅有一个对应的 set 操作触发。当 `WaitFlag` 等待的 event 从未被 `SetFlag` 触发时，该等待操作将永远不会返回，导致当前 AI Core 上的 kernel 执行线程永久阻塞（cross_references "症状相似"段：kernel hangs = {flag_mismatch, alloc_free_mismatch, enque_deque_mismatch, padding_misconfig}，区分要点：flag_mismatch = flag 不配对，SyncBlockWait 无配对 SyncBlockArrive）。

触发条件（detector 原文，逐字保留）：

- `WaitFlag`（或 `asc_sync_block_wait`）等待的 event ID 未被任何 `SetFlag`（或 `asc_sync_block_set`）触发
- 条件分支（if/else）中某些路径缺少 `SetFlag`，导致在特定条件下 WaitFlag 永久等待
- 多循环嵌套中 `SetFlag` 调用次数与 `WaitFlag` 调用次数不一致
- SetFlag 和 WaitFlag 使用了不同的 Event ID，导致信号错位

形式化规则 flag_mismatch-F01 前提：`∀ eventId E: COUNT(SetFlag(E)) == COUNT(WaitFlag(E))`。

## 检测规则

### flag_mismatch-RULE-01：孤立 WaitFlag 检测

**【条件】**函数或代码块中 `asc_sync_block_wait(eventId)` 或 `WaitFlag(eventId, ...)` 出现，但整个编译单元内不存在对应的 `asc_sync_block_set(eventId)` 或 `SetFlag(eventId, ...)`
**【API】**asc_sync_block_wait / WaitFlag / asc_sync_block_set / SetFlag
**【约束】**对每个 WaitFlag 调用中的 eventId 常量值，整个源文件（及相关 include 文件）中存在使用相同 eventId 的 SetFlag 调用
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

匹配示例（detector 原文）：

```cpp
asc_sync_block_wait(5);   // eventId=5，但无 SetFlag(5)
```

### flag_mismatch-RULE-02：条件分支路径覆盖检测

**【条件】**`SetFlag` 出现在 if/else 的某个分支中，但 `WaitFlag` 在分支汇合点之后执行，且并非所有分支路径都包含 `SetFlag` 调用
**【API】**SetFlag / WaitFlag（控制流图前驱路径分析）
**【约束】**对控制流图（CFG）进行分析，WaitFlag 节点的所有前驱路径上都存在使用相同 eventId 的 SetFlag 调用
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

匹配示例（detector 原文）：

```cpp
if (mode == 0) {
    SetFlag(3, val);       // 仅 mode==0 路径有 SetFlag
}
// else 分支无 SetFlag(3)
asc_sync_block_wait(3);   // 潜在死锁
```

### flag_mismatch-RULE-03：Event ID 一致性检测

**【条件】**`SetFlag` 和 `WaitFlag` 调用均存在，但使用的 eventId 不一致
**【API】**SetFlag / WaitFlag（eventId 参数集合比对）
**【约束】**所有 WaitFlag 调用的每个 eventId 在 SetFlag 调用的 eventId 集合中存在对应项
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
SetFlag(3, val);          // eventId=3
asc_sync_block_wait(5);   // eventId=5，不匹配
```

### flag_mismatch-RULE-04：循环嵌套计数一致性

**【条件】**循环结构包含 SetFlag(E) 或 WaitFlag(E)，循环内 `SetFlag(E) 的迭代次数 × 单次迭代调用次数 ≠ WaitFlag(E) 的迭代次数 × 单次迭代调用次数`
**【API】**SetFlag / WaitFlag（循环内调用计数）
**【约束】**`loop_set_count == loop_wait_count`（含循环迭代次数乘以单次迭代调用次数）
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

### flag_mismatch-RULE-05：动态调用次数匹配（运行时检测）

**【条件】**运行时插桩统计每个 eventId 上 `SetFlag` 和 `WaitFlag` 的实际调用次数，某 eventId 上 WaitFlag 调用次数大于 SetFlag 调用次数
**【API】**SetFlag / WaitFlag（运行时计数器以 eventId 为 key）
**【约束】**`∀ eventId E: COUNT(SetFlag(E)) == COUNT(WaitFlag(E))`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

### flag_mismatch-RULE-06：超时检测（运行时检测）

**【条件】**为 `WaitFlag` 设置超时阈值（如 10 秒），超时未返回
**【API】**WaitFlag（host 端 watchdog 或 device 端硬件定时器）
**【约束】**WaitFlag 调用需在合理阈值内返回（hang 即命中）
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

## 修复方案

### 标准修复模式

1. **修复模式 flag_mismatch-FIX-01（补全缺失的 SetFlag）**：WaitFlag 存在但缺少对应 SetFlag 时补全配对调用
2. **修复模式 flag_mismatch-FIX-02（条件分支路径补全）**：if/else 分支中部分路径缺少 SetFlag 时，在每个分支末尾补全
3. **修复模式 flag_mismatch-FIX-03（多循环嵌套 flag 配对修正）**：外层循环调用 WaitFlag 但内层 SetFlag 次数不匹配时，调整循环内调用次数使 set/wait 配对
4. **修复模式 flag_mismatch-FIX-04（Event ID 一致性修正）**：SetFlag 和 WaitFlag 使用不同 eventId 时统一为同一值

### 代码模板

模板 flag_mismatch-TPL-01：标准 SetFlag/WaitFlag 配对模式（detector 原文）

```cpp
template<typename T>
void process_with_sync(int eventId) {
    SetFlag(eventId, FLAG_SET);
    asc_sync_block_wait(eventId);
}
```

模板 flag_mismatch-TPL-02：条件安全 flag 操作模板（detector 原文）

```cpp
template<typename T>
void conditional_sync(int eventId, bool condition) {
    if (condition) {
        SetFlag(eventId, FLAG_SET);
    }
    if (condition) {
        asc_sync_block_wait(eventId);
    }
}
```

### 防御性编程建议

1. **强制配对规范**：编码规范中要求 SetFlag 和 WaitFlag 必须在同一函数或同一逻辑块中成对出现，禁止跨越多个函数层级进行隐式配对。
2. **Event ID 常量化**：将 eventId 定义为具名常量（如 `const int SYNC_EVENT_DATA_READY = 5;`），避免硬编码数字导致的 ID 不一致问题。
3. **静态分析工具集成**：在 CI/CD 流水线中集成静态分析工具，自动检测 SetFlag/WaitFlag 的配对完整性，包括条件分支路径覆盖分析。
4. **代码审查检查项**：在代码审查（Code Review）流程中加入 SetFlag/WaitFlag 配对性的专项检查项，尤其关注新增条件分支和循环结构。
5. **统一同步封装**：封装统一的同步原语（如 `SyncBarrier` 类），在构造函数中注册 SetFlag，析构函数中验证 WaitFlag 是否完成，利用 RAII 机制确保配对。
6. **超时保护机制**：在 host 端设置 kernel 执行超时监控，当 kernel 执行时间超过阈值时自动终止并报告疑似 flag_mismatch 类 hang 异常。

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| 缺少 SetFlag，只有 WaitFlag | cann-aicore/03_sync/flag_mismatch_hangs | `asc_sync_block_wait(5)` 无任何 SetFlag(5) | EC1 |
| 条件分支部分路径缺少 SetFlag | 变体（待生成） | if/else 仅部分分支调用 SetFlag | EC2 |
| 多循环嵌套次数不匹配 | 变体（待生成） | 外层循环 N 次 WaitFlag 但内层仅 M 次 SetFlag（N≠M） | EC3 |
| Event ID 不一致 | 变体（待生成） | SetFlag(3) 配 WaitFlag(5)，eventId 错位 | EC4 |

## 等价类划分

变异维度：SetFlag/WaitFlag 配对完整性 + 控制流路径覆盖

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | 完全缺失 SetFlag（孤立 WaitFlag） | set_count = 0 | 最简单配对缺失，覆盖原始种子 |
| EC2 | 条件分支部分路径缺失 | 部分前驱路径无 SetFlag | 需 CFG 分析 |
| EC3 | 循环嵌套次数不匹配 | loop_set_count ≠ loop_wait_count | 需循环迭代分析 |
| EC4 | eventId 错位 | SetFlag 集合 ∩ WaitFlag 集合 = ∅ | eventId 值不匹配 |

划分依据：detector detection_rules "变体检测覆盖"表 4 类变体一一对应（可检测性由高到低：EC1/EC4 静态完全可检测，EC2/EC3 需 CFG/循环分析）。

## 关联异常

- **alloc_free_mismatch / enque_deque_mismatch / padding_misconfig**：同属 cross_references "症状相似"段：kernel hangs = {flag_mismatch, alloc_free_mismatch, enque_deque_mismatch, padding_misconfig}。区分要点（cross_references 原文）：flag_mismatch = flag 不配对（SyncBlockWait 无配对 SyncBlockArrive）；alloc_free_mismatch = AllocTensor 后无 FreeTensor；enque_deque_mismatch = EnQue 后无 DeQue；padding_misconfig = 有错误码 41 但实际挂起，LoadData3D padding 配置导致 fmap < filter。
- **PipeBarrier / SyncBlockWait / SyncBlockArrive / set_flag / wait_flag**：共享同步控制类 API（cross_references 反查 api_index：PipeBarrier → [flag_mismatch, alloc_free_mismatch, enque_deque_mismatch]，SyncBlockWait/SyncBlockArrive/set_flag/wait_flag → [flag_mismatch]）。
- **（推断，非 cross_references 收录）**与 alloc_free_mismatch/enque_deque_mismatch 共享"配对缺失导致 kernel 永久阻塞"语义模式，但触发 API 不同（flag_mismatch = flag、alloc_free_mismatch = AllocTensor/FreeTensor、enque_deque_mismatch = EnQue/DeQue）。

## 检测执行指引

- **适用匹配模式**：存在性检查（RULE-01/06）、API 调用模式（RULE-02/04/05）、值域判断（RULE-03）
- **命中判定**：
  - 存在性检查类规则——全文模式搜索 `asc_sync_block_wait(E)` / `WaitFlag(E)`，对每个 eventId 常量在源文件及 include 范围内反查是否存在 `SetFlag(E)` / `asc_sync_block_set(E)`，缺失即命中；跨行追溯 ≤3 层
  - API 调用模式类规则——构建调用序列（SetFlag/WaitFlag 调用顺序、循环体内调用计数），统计每个 eventId 上的 set/wait 次数；CFG 前驱路径分析时若跨函数追溯受限，标记 `trace_limited=true`
  - 值域判断类规则——提取 SetFlag/WaitFlag 调用的 eventId 实参常量，比对两集合；不交立即命中
- **修复映射**：
  - flag_mismatch-RULE-01（孤立 WaitFlag）→ 修复模式 flag_mismatch-FIX-01（补全 SetFlag）
  - flag_mismatch-RULE-02（条件分支路径覆盖）→ 修复模式 flag_mismatch-FIX-02（条件分支补全）
  - flag_mismatch-RULE-03（Event ID 一致性）→ 修复模式 flag_mismatch-FIX-04（Event ID 修正）
  - flag_mismatch-RULE-04（循环嵌套计数）→ 修复模式 flag_mismatch-FIX-03（多循环配对修正）
  - flag_mismatch-RULE-05（动态调用次数匹配）→ 修复模式 flag_mismatch-FIX-03（用于运行时定位）
  - flag_mismatch-RULE-06（超时检测）→ 用于诊断路由，不直接触发代码修复
