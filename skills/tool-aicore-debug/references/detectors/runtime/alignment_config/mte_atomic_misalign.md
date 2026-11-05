# mte_atomic_misalign — MTE 原子加地址非对齐

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | mte_atomic_misalign |
| 硬件事件编号 | 11（L30 ATM_ADD） |
| 异常类型 | MTE 原子加地址非对齐（配置非法类异常） |
| 错误码 | errcode:(TBD, TBD, 0) ATM_ADD 类；subErrType=4 |
| 类别 | alignment_config |
| 源文件 | error/main.asc:32（baseAddr + 2ULL 偏移） |
| 报错模式 | EZ9999 + retCode=0x31 [vector core exception] + subErrType=4 |
| 内存区域 | GM（GlobalMemory，SetAtomicAdd 写目标） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 常量注入（偏移 2ULL→0ULL）+ return 1→0 |
| behavior_note | SetAtomicAdd + DataCopy 写 GM，目标地址 +2 字节破坏 32B 对齐 |

## 触发条件

**违规模式**：SetAtomicAdd + DataCopy 写 GM 路径，目标 GM 地址 = `baseAddr + 2ULL`（baseAddr 本身 32B 对齐），偏移 +2 字节破坏 32B 对齐 → AICoreError.md L30 ATM_ADD（"mte atomic add地址不对齐"）。

**安全模式**：目标 GM 地址 = `baseAddr + 0ULL`（即 baseAddr 本身），32B 对齐。

## 检测规则

### mte_atomic_misalign-RULE-01：AtomicAdd 目标地址非 32B 对齐

- 【条件】SetAtomicAdd 状态下，DataCopy 写目标 GlobalTensor 的物理地址非 32B 对齐
- 【API】`SetAtomicAdd<float>(); ... DataCopy(badGM, xLocal, TOTAL_LENGTH);`
- 【约束】AtomicAdd 要求 GM 写目标地址 32B 对齐；baseAddr+2 → (addr) mod 32 = 2 ≠ 0 → 非对齐
- 【匹配模式】值域判断

### mte_atomic_misalign-RULE-02：偏移常量注入（+2ULL）

- 【条件】GlobalTensor 地址通过 `baseAddr + 2ULL` 构造，破坏 32B 对齐
- 【API】`reinterpret_cast<__gm__ float*>(reinterpret_cast<uintptr_t>(baseAddr) + 2ULL)`
- 【约束】baseAddr 32B 对齐，+2 后 (addr+2) mod 32 = 2 → 违反对齐
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：将偏移常量从 2ULL 改为 0ULL，使用 baseAddr 本身（32B 对齐）。

```cpp
// 修复前（error）
reinterpret_cast<__gm__ float*>(reinterpret_cast<uintptr_t>(baseAddr) + 2ULL)

// 修复后（fixed）
reinterpret_cast<__gm__ float*>(reinterpret_cast<uintptr_t>(baseAddr) + 0ULL)
```

**防御性编程建议**：

- AtomicAdd 写目标前断言 `(reinterpret_cast<uintptr_t>(dst) % 32 == 0)`
- 封装 AtomicAdd-safe 的 DataCopy wrapper，强制对齐检查

## 变体索引

| 变体 | 偏移量 | (addr) mod 32 | 对齐状态 |
|------|--------|---------------|---------|
| 基准（本用例） | +2 | 2 | 非对齐 |
| 小偏移变体 | +1 | 1 | 非对齐 |
| 对齐变体 | +32 | 0 | 对齐 |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：AtomicAdd 目标 32B 对齐 | (baseAddr + offset) mod 32 == 0 | fixed 版（+0ULL） |
| EC2：AtomicAdd 目标非对齐 | (baseAddr + offset) mod 32 != 0 | error 版（+2ULL） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| fixpipe_addr_misalign | 同族地址对齐类异常，Fixpipe 路径 |
| cube_instr_illegal (L9) | 同族地址对齐类异常，Cube MMAD 路径 |

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01）、diff_pattern（RULE-02）
- **命中判定**：RULE-01 核对 AtomicAdd 状态下 DataCopy 写目标的对齐，RULE-02 核对地址偏移注入；应确认偏移后的 GlobalTensor 用于该 DataCopy。
  - 读取 error/main.asc 第 28 行 `SetAtomicAdd<float>()` 确认原子加模式。
  - 读取第 32 行 `reinterpret_cast<uintptr_t>(baseAddr) + 2ULL` → 偏移 +2。
  - 计算 baseAddr（32B 对齐）+ 2 → mod 32 = 2 ≠ 0 → 非对齐。
  - error 版 stderr 应含 EZ9999 + retCode=0x31 + subErrType=4。
- **修复映射**：
  - mte_atomic_misalign-RULE-01 → 恢复 AtomicAdd 写目标地址的 32B 对齐。
  - mte_atomic_misalign-RULE-02 → 将地址偏移 `+ 2ULL` 改为 fixed 版的 `+ 0ULL`；baseAddr mod 32 = 0，恢复对齐。
