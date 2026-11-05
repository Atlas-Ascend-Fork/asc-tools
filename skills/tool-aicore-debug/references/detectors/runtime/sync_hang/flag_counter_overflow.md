# flag_counter_overflow — Flag counter overflow（核内 SetFlag counter 累积探索 L101）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | flag_counter_overflow |
| 硬件事件编号 | L101 |
| 异常类型 | Flag counter overflow（探索：核内 HardEvent SetFlag counter 累积试探核间 L101 counter） |
| 置信度 | 低（L101 触发为未验证假设，预期高概率剔除） |
| 需求目标 | AICoreError.md L101 核间通讯Flag counter overflow — EventID 0x80C98002 |
| 实际覆盖 | 探索 L101 — tile 2 循环 16 次 `SetFlag<MTE2_V>(7)` 不 WaitFlag，假设核内 counter 与 L101 核间 counter 共用（max=15） |
| 错误码 | errcode:TBD（flag counter overflow 无先例） |
| 类别 | sync_hang |
| 源文件 | error/main.asc:10（`FC_TRIGGER_TILE = 2` 武装开关）+ kernel_class.h/fault_injector.h（注入逻辑） |
| 报错模式 | EZ9999（验证阶段补充 retCode/errcode 字段；#2 预期高概率剔除） |
| 内存区域 | 核内管线同步 counter（SetFlag<MTE2_V> 累积） |
| API层级 | TC-High |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 常量注入（FC_TRIGGER_TILE=2→0xFFFFFFFFu 哨兵解除武装）+ return 1→0；fault_injector.h/kernel_class.h 两侧逐字相同 |
| behavior_note | 多文件结构（main.asc+kernel_class.h+fault_injector.h），tile 2 时注入器 16 次 `SetFlag<MTE2_V>(7)` 不 WaitFlag，假设核内 counter 与核间 L101 共用（max=15）→ 溢出 |

## 触发条件

**违规模式**：TC-High kernel class + 多文件结构（main.asc + kernel_class.h + fault_injector.h）。`KernelFlagCounterOverflow` 执行 4-tile VEC 流水线，每 tile 调 `FaultInjector::MaybeInjectFlagOverflow(t)`。main.asc 顶部武装开关 `FC_TRIGGER_TILE=2` 经 `Init(x,y,z, FC_TRIGGER_TILE)` 转交注入器构造；当 `currentTile==2` 时进入注入分支循环 16 次 `SetFlag<HardEvent::MTE2_V>(7)`（eventId=7 ∈ [0, QUE_MAX_EVENT=8) 合法）不配对 WaitFlag，`volatile int sink` 每轮写入阻止编译器合并。**核心假设**：核内管线同步 counter 与 L101 核间 counter 共用同一物理计数器（max=15），16 次累积 > 15 → L101。

**安全模式**：`FC_TRIGGER_TILE=0xFFFFFFFFu`（哨兵，与 tile 0..3 均不等 → 永不武装 → 无 SetFlag → counter 不累积），即 fault-injection 框架标准"生产解除武装"修复模式。fault_injector.h 与 kernel_class.h 在 error/fixed 逐字相同（注入库两侧一致），全部 diff 落在 main.asc 的 FC_TRIGGER_TILE 武装开关值 + return。

## 检测规则

### flag_counter_overflow-RULE-01：SetFlag 循环不配对 WaitFlag 致 counter 累积

- 【条件】`SetFlag<HardEvent::MTE2_V>` 调用次数（16）超过 counter 上限（max=15），且不配对 WaitFlag
- 【API】`for (...) { SetFlag<HardEvent::MTE2_V>(7); volatile int sink = ...; }`（16 次不 WaitFlag）
- 【约束】核内管线同步 counter 假设上限 15；16 次 SetFlag 不 WaitFlag → counter 累积 > 15 → L101（未验证假设）
- 【匹配模式】API 调用模式

### flag_counter_overflow-RULE-02：FC_TRIGGER_TILE 武装开关注入

- 【条件】FC_TRIGGER_TILE 从 2 改为 0xFFFFFFFFu（哨兵解除武装）
- 【API】`constexpr uint32_t FC_TRIGGER_TILE = 2;`（error）vs `constexpr uint32_t FC_TRIGGER_TILE = 0xFFFFFFFFu;`（fixed）
- 【约束】error 版 FC_TRIGGER_TILE=2 → tile 2 时武装注入器执行 16 次 SetFlag；fixed 版 0xFFFFFFFFu → 哨兵值永不匹配 tile → 永不武装 → 无 SetFlag
- 【匹配模式】diff_pattern

### flag_counter_overflow-RULE-03：eventId 合法性 + 核内/核间 counter 共用假设

- 【条件】eventId=7 ∈ [0, QUE_MAX_EVENT=8) 合法，但核内 counter 与 L101 核间 counter 共用为未验证假设
- 【API】`SetFlag<HardEvent::MTE2_V>(7)`（eventId=7）
- 【约束】eventId=7 合法（∈ [0,8)）；核心假设：核内 SetFlag counter 与 L101 核间 counter 共用同一物理 counter（max=15）；该假设无技术证据，验证阶段实证
- 【匹配模式】值域判断

## 修复方案

**标准修复模式**：将 FC_TRIGGER_TILE 武装开关设为哨兵值 0xFFFFFFFFu（永不匹配 tile → 永不武装 → 无 SetFlag → counter 不累积），fault_injector.h/kernel_class.h 与 error 版逐字相同。

```cpp
// 修复前（error）
constexpr uint32_t FC_TRIGGER_TILE = 2;

// 修复后（fixed）
constexpr uint32_t FC_TRIGGER_TILE = 0xFFFFFFFFu;
```

**防御性编程建议**：

- SetFlag/WaitFlag 严格配对，避免 counter 累积
- fault-injection 武装开关使用哨兵值（0xFFFFFFFFu）在生产环境解除武装
- 核内管线同步 counter 与核间 counter 是否共用需芯片手册确认

## 变体索引

| 变体 | FC_TRIGGER_TILE | SetFlag 次数 | waitFlag | 触发路径 |
|------|----------------|-------------|----------|---------|
| 基准（本用例） | 2 | 16 | 无 | L101（未验证假设） |
| 证伪变体 | 2 | 16 | 无 | NO_EZ9999 / RUN_TIMEOUT（counter 不共用） |
| 合法变体 | 0xFFFFFFFFu | 0 | — | 安全（fixed 版，哨兵解除武装） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：FC_TRIGGER_TILE 哨兵解除武装 | FC_TRIGGER_TILE ∉ tile 范围 → 永不武装 | fixed 版（0xFFFFFFFFu） |
| EC2：FC_TRIGGER_TILE 武装注入 | FC_TRIGGER_TILE ∈ tile 范围 → 武装 SetFlag 累积 | error 版（FC_TRIGGER_TILE=2） |
| EC3：L101 假设证伪 | counter 不共用 / errcode 无关 → 剔除 | 验证阶段实证 |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| hset_counter_overflow (L8) | 同族 counter 溢出探索，HSET arrive counter 路径 |
| vec_neg_sqrt | 同骨架 TC-High kernel class，来源种子 |
| sync_flag_mismatch_hang | 同族同步异常，flag 不匹配路径 |

## 检测执行指引

- **适用匹配模式**：API 调用模式（RULE-01）、diff_pattern（RULE-02）、值域判断（RULE-03）。
- **命中判定**：按下列步骤核对 RULE-01 的 16 次 SetFlag 无 WaitFlag、RULE-02 的武装开关差异；RULE-03 确认 `eventId=7 ∈ [0,8)` 合法，并保留核内/核间 counter 共用及 max=15 的未验证假设。
  1. 读取 error/main.asc 第 10 行 `FC_TRIGGER_TILE = 2` 确认武装开关。
  2. 读取 kernel_class.h/fault_injector.h 确认注入逻辑（tile 2 时 16 次 `SetFlag<MTE2_V>(7)` 不 WaitFlag）；确认两侧逐字相同。
  3. 计算 FC_TRIGGER_TILE=2 → tile 2 武装 → 16 次 SetFlag > counter max 15 → L101（未验证假设）。
  4. fixed 版第 10 行 `FC_TRIGGER_TILE = 0xFFFFFFFFu`（哨兵永不匹配）→ 无 SetFlag → 安全。
  5. error 版 stderr 应含 EZ9999（验证阶段补充 retCode/errcode）；若证伪（counter 不共用）→ NO_EZ9999 / RUN_TIMEOUT / errcode 无关 → 剔除。
  - `runtime_required=true`：L101 归属和 counter 共用假设必须经运行时 retCode/errcode 验证；若出现 NO_EZ9999、RUN_TIMEOUT 或无关 errcode，应按原证伪流程剔除该假设，不将 16 次调用直接作为 L101 已确认命中。
- **修复映射**：RULE-01 → SetFlag/WaitFlag 严格配对，避免 counter 累积；RULE-02 → 种子修复将 `FC_TRIGGER_TILE` 设为 `0xFFFFFFFFu` 解除武装，保持两个头文件与 error 版逐字相同；RULE-03 → eventId=7 本身无需修改，counter 共用假设待芯片手册及运行时证据确认，证伪时剔除对应 L101 归因。
