# cube_l0a_wraparound — CUBE L0A 卷绕越界

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | cube_l0a_wraparound |
| 硬件事件编号 | L15 |
| 异常类型 | CUBE L0A 卷绕越界 |
| 错误码 | errcode:TBD（L15 与 L13 同族 cube core 路径） |
| 类别 | cube |
| 源文件 | error/main.asc:23（asc_copy_l12l0a 参数注入） |
| 报错模式 | EZ9999 + retCode=0x26 [aicore exception] + cube error info 非零 |
| 内存区域 | L0A（`__ca__` half*，dav-2201 = 64KB = 32768 half） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | API 参数注入（repeat/dst_gap）+ return 1→0 |
| behavior_note | dst_gap=8192 × repeat=8 = 65536 half = 128KB ≫ L0A 64KB → 写址回转 |

## 触发条件

> **容量适用范围**：本文容量数值记录历史种子环境，不代表所有芯片或 tensor 的可访问容量。当前输入须先确认架构、API 重载及实际分配边界；未知时标记 `runtime_required=true`。统一单位与计分口径见 `references/constraints/semantic_contract.md`。

**违规模式**：`asc_copy_l12l0a(aL0a, aL1, 0, 8, 0, 8192)` 中 dst_gap=8192 half（16KB）/步 × repeat=8 = 65536 half（128KB），写址落在 L0A 偏移 0/16KB/32KB/48KB/64KB(回转)，第 5 块起跨越 dav-2201 L0A 64KB 边界回转 → L15。src_stride=0 保证 8 次均读同一 L1 源块，故障隔离到 L0A 写侧。

**安全模式**：`asc_copy_l12l0a(aL0a, aL1, 0, 1, 1, 0)`，repeat=1/dst_gap=0 单次写不越界。

## 检测规则

### cube_l0a_wraparound-RULE-01：asc_copy_l12l0a dst_gap×repeat 超 L0A 容量

- 【条件】`asc_copy_l12l0a` 的 dst_gap（half 单位）× repeat（次数）超过 L0A 容量 32768 half（64KB）
- 【API】`asc_copy_l12l0a(dst, src, start_index, repeat, src_stride, dst_gap)`
- 【约束】L0A 容量 = 32768 half（dav-2201 = 64KB）；dst_gap=8192, repeat=8 → 8×8192=65536 > 32768 → 越界
- 【匹配模式】值域判断

### cube_l0a_wraparound-RULE-02：(repeat-1)×dst_gap 超 L0A 容量（末址模型）

- 【条件】(repeat-1)×dst_gap 超过 L0A 容量边界
- 【API】`asc_copy_l12l0a(dst, src, start_index, repeat, src_stride, dst_gap)`
- 【约束】末块写址 = start_index + (repeat-1)×dst_gap；(8-1)×8192=57344 half=112KB > 64KB → 越界
- 【匹配模式】值域判断

## 修复方案

**标准修复模式**：将 repeat 降为 1、dst_gap 降为 0，使单次写不越界。

```cpp
// 修复前（error）
asc_copy_l12l0a(aL0a, aL1, 0, 8, 0, 8192);

// 修复后（fixed）
asc_copy_l12l0a(aL0a, aL1, 0, 1, 1, 0);
```

**防御性编程建议**：

- 调用前断言 `repeat * dst_gap * sizeof(half) <= L0A_SIZE_BYTES`（65536）
- 封装带边界检查的 asc_copy_l12l0a wrapper

## 变体索引

| 变体 | repeat | dst_gap | repeat×dst_gap | 越界量 |
|------|--------|---------|----------------|--------|
| 基准（本用例） | 8 | 8192 | 65536 half=128KB | +64KB |
| 小 repeat 变体 | 4 | 8192 | 32768 half=64KB | 恰到边界 |
| 大 dst_gap 变体 | 8 | 16384 | 131072 half=256KB | +192KB |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：L0A 写址安全 | repeat×dst_gap×2 ≤ 65536 | fixed 版（1×0=0） |
| EC2：L0A 写址回转 | repeat×dst_gap×2 > 65536 | error 版（8×8192=65536 half） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| cube_l0b_wraparound (L14) | 同族，L0B 写址回转，同 dst_gap×repeat 机制 |
| cube_l0c_overflow (L13) | 同族，L0C 写址回转（单次大偏移路径） |
| mte1_write_overflow (L56) | asc_copy_l12l0b 写侧越界（MTE1 写族） |

## 检测执行指引

- **适用匹配模式**：值域判断（`cube_l0a_wraparound-RULE-01`）；值域判断（`cube_l0a_wraparound-RULE-02`）。
- **命中判定**：按 RULE-01 核对 repeat×dst_gap，按 RULE-02 进一步核对末块写址 `start_index + (repeat-1)×dst_gap`；种子末块偏移为 `(8-1)×8192=57344 half`，超过本文的 L0A 容量 32768 half。保留以下取值、修复对照和日志核对步骤：

  1. 读取 error/main.asc 第 23 行 `asc_copy_l12l0a(aL0a, aL1, 0, 8, 0, 8192)`。
  2. 提取 repeat=8, dst_gap=8192，计算 8×8192=65536 half=128KB > L0A 64KB → 回转。
  3. fixed 版第 23 行 repeat=1, dst_gap=0 → 1×0=0 ≤ 64KB → 安全。
  4. error 版 stderr 应含 EZ9999 + retCode=0x26 + cube error info 非零。
- **修复映射**：
  - `cube_l0a_wraparound-RULE-01`、`cube_l0a_wraparound-RULE-02` → 按种子修复将调用恢复为 `asc_copy_l12l0a(aL0a, aL1, 0, 1, 1, 0)`，即 repeat=1、src_stride=1、dst_gap=0，并重新核对写址边界。
