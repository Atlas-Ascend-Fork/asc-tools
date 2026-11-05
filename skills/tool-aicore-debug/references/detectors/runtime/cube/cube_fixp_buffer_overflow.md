# cube_fixp_buffer_overflow — CUBE L0C 卷绕越界（loop 累积路径）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | cube_fixp_buffer_overflow |
| 硬件事件编号 | L13（需求名 L6，经 `__cc__` 不可达） |
| 异常类型 | CUBE L0C 卷绕越界（loop 累积路径） |
| 错误码 | errcode:TBD（L13 族，与 cube_l0c_overflow 同族不同机制） |
| 类别 | cube |
| 源文件 | error/main.asc:26-28（for loop + asc_mmad 偏移递增） |
| 报错模式 | EZ9999 + retCode=0x26 [aicore exception] + aic error mask=0x6500020bd00028c |
| 内存区域 | L0C（`__cc__` float*，dav-2201 = 128KB = 32768 floats） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 结构变更（for loop 160 次→单次调用）+ return 1→0 |
| behavior_note | loop 160 次 asc_mmad 累积 c 指针步长 256 floats，i≥128 起跨 128KB L0C 边界回转 |

## 触发条件

> **容量适用范围**：本文容量数值记录历史种子环境，不代表所有芯片或 tensor 的可访问容量。当前输入须先确认架构、API 重载及实际分配边界；未知时标记 `runtime_required=true`。统一单位与计分口径见 `references/constraints/semantic_contract.md`。

**违规模式**：asc_mmad 外层 loop 160 次（LOOP_COUNT=160），每次 c_matrix 指针递增 LOOP_STEP=256 floats（M×N=16×16），累积地址超 dav-2201 L0C 128KB=32768 floats 容量 → 地址回转到 0 → L13。i=128 时写地址 [32768, 33024) 首地址恰为 L0C 容量边界 → 回转；i=128..159 持续跨边界写。

**安全模式**：单次 `asc_mmad(cL0c, ...)`，写址 [0, 256) 全在 L0C 内。

## 检测规则

### cube_fixp_buffer_overflow-RULE-01：asc_mmad loop 累积偏移跨 L0C 边界

- 【条件】asc_mmad 在 for loop 中多次调用，c_matrix 指针按 i×LOOP_STEP 累积，当 i×LOOP_STEP ≥ L0C 容量时越界
- 【API】`for (int i = 0; i < LOOP_COUNT; i++) { asc_mmad(cL0c + i * LOOP_STEP, ...) }`
- 【约束】L0C 容量 = 32768 floats；LOOP_STEP=256, LOOP_COUNT=160 → i=128 时偏移 32768 = 容量边界 → 回转
- 【匹配模式】值域判断

### cube_fixp_buffer_overflow-RULE-02：loop 结构移除（单次调用）

- 【条件】error 版含 for loop，fixed 版移除 loop 改为单次 asc_mmad 调用
- 【API】error: `for (...) { asc_mmad(cL0c + i * LOOP_STEP, ...) }`；fixed: `asc_mmad(cL0c, ...)`
- 【约束】单次调用写址 [0, 256) ⊂ L0C → 安全
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：移除 for loop，改为单次 asc_mmad 调用，使用合法基址。

```cpp
// 修复前（error）
for (int i = 0; i < (int)LOOP_COUNT; i++) {
    asc_mmad(cL0c + i * LOOP_STEP, aL0a, bL0b, M, N, K, 0, false, false, false);
}

// 修复后（fixed）
asc_mmad(cL0c, aL0a, bL0b, M, N, K, 0, false, false, false);
```

**防御性编程建议**：

- 循环内对 c_matrix 偏移做模运算：`offset = (i * LOOP_STEP) % L0C_SIZE_FLOATS`
- 调用前断言 `i * LOOP_STEP + M * N <= L0C_SIZE_FLOATS`

## 变体索引

| 变体 | LOOP_COUNT | LOOP_STEP | i=128 时偏移 | 越界 |
|------|-----------|-----------|-------------|------|
| 基准（本用例） | 160 | 256 | 32768 | 恰到边界 |
| 小 loop 变体 | 128 | 256 | 32768 | 恰到边界（i=127） |
| 单次大偏移变体 | 1 | 32640 | — | 见 cube_l0c_overflow |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：单次写安全 | offset + m×n ≤ 32768 | fixed 版（0+256=256） |
| EC2：loop 累积越界 | LOOP_COUNT×LOOP_STEP > 32768 | error 版（160×256=40960） |
| EC3：单次大偏移越界 | 单次 offset+m×n > 32768 | cube_l0c_overflow |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| cube_l0c_overflow (L13) | 同族 L13，单次大偏移路径（本用例为 loop 累积路径） |
| cube_l0a_wraparound (L15) | 同族 cube core，L0A 回转 |
| cube_l0b_wraparound (L14) | 同族 cube core，L0B 回转 |

## 检测执行指引

- **适用匹配模式**：值域判断（`cube_fixp_buffer_overflow-RULE-01`）；diff_pattern（`cube_fixp_buffer_overflow-RULE-02`）。
- **命中判定**：按 RULE-01 核对循环次数、步长和 L0C 容量，确认累计写地址跨界；按 RULE-02 对照 error/fixed 的循环移除差异。保留以下取值、计算和日志核对步骤：

  1. 读取 error/main.asc 第 9 行 `LOOP_COUNT=160`、第 10 行 `LOOP_STEP=M*N=256`。
  2. 确认第 26-28 行 for loop 包裹 asc_mmad，偏移 `cL0c + i * LOOP_STEP`。
  3. 计算 160×256=40960 > L0C 32768 floats → i=128 起回转。
  4. fixed 版第 26 行单次 `asc_mmad(cL0c, ...)` → 0+256=256 ≤ 32768 → 安全。
  5. error 版 stderr 应含 EZ9999 + retCode=0x26 + aic error mask=0x6500020bd00028c。
- **修复映射**：
  - `cube_fixp_buffer_overflow-RULE-01`、`cube_fixp_buffer_overflow-RULE-02` → 移除种子中的 for loop，使用 `asc_mmad(cL0c, ...)` 单次调用；按修复方案核对写址 `[0, 256)` 位于 L0C 内。
