# cube_l0c_overflow — CUBE L0C 卷绕越界（单次大偏移路径）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | cube_l0c_overflow |
| 硬件事件编号 | L13 |
| 异常类型 | CUBE L0C 卷绕越界（单次大偏移路径） |
| 错误码 | errcode:TBD（L13 无先例）；EventID 0x80C98002 |
| 类别 | cube |
| 源文件 | error/main.asc:20（`__cc__ float* cBad = cL0c + C_BAD_OFFSET`） |
| 报错模式 | EZ9999 + retCode=0x26 [aicore exception] + aic error mask=0x6500020bd00028c |
| 内存区域 | L0C（`__cc__` float*，dav-2201 = 128KB = 32768 floats） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 指针偏移常量注入（C_BAD_OFFSET=32640→0）+ return 1→0 |
| behavior_note | 单次 asc_mmad 写 cL0c+32640，m×n=256 floats 写入 [32640,32896) 跨 128KB 边界 128 floats → 回转 |

## 触发条件

> **容量适用范围**：本文容量数值记录历史种子环境，不代表所有芯片或 tensor 的可访问容量。当前输入须先确认架构、API 重载及实际分配边界；未知时标记 `runtime_required=true`。统一单位与计分口径见 `references/constraints/semantic_contract.md`。

**违规模式**：`__cc__ float* cBad = cL0c + 32640`（C_BAD_OFFSET=32640），`asc_mmad(cBad, ...)` 单次写入 m×n=16×16=256 floats，覆盖 [32640, 32896) floats，跨越 dav-2201 L0C 128KB=32768 floats 边界 128 floats → 地址回转到 0 → L13。

**安全模式**：`__cc__ float* cBad = cL0c`（无偏移），写址 [0, 256) 全在 L0C 内。

## 检测规则

### cube_l0c_overflow-RULE-01：asc_mmad c_matrix 偏移 + m×n 跨 L0C 边界

- 【条件】asc_mmad 的 c_matrix 指针偏移（float 元素数）+ m×n > L0C 容量 32768 floats
- 【API】`asc_mmad(cBad, aL0a, bL0b, M, N, K, 0, false, false, false)`；cBad = cL0c + C_BAD_OFFSET
- 【约束】L0C 容量 = 32768 floats（dav-2201 = 128KB）；C_BAD_OFFSET=32640, M×N=256 → 32640+256=32896 > 32768 → 越界 128 floats
- 【匹配模式】值域判断

### cube_l0c_overflow-RULE-02：__cc__ 指针偏移常量注入

- 【条件】`__cc__ float*` 指针通过 `+ C_BAD_OFFSET` 构造，C_BAD_OFFSET 非零且接近容量边界
- 【API】`__cc__ float* cBad = cL0c + C_BAD_OFFSET;`
- 【约束】C_BAD_OFFSET=32640 距 L0C 上界 32768 仅 128 floats 余量 → 单次写必越界
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：移除 c_matrix 指针偏移，使用合法基址。

```cpp
// 修复前（error）
constexpr uint32_t C_BAD_OFFSET = 32640;
__cc__ float* cBad = cL0c + C_BAD_OFFSET;
asc_mmad(cBad, aL0a, bL0b, M, N, K, 0, false, false, false);

// 修复后（fixed）
__cc__ float* cBad = cL0c;
asc_mmad(cBad, aL0a, bL0b, M, N, K, 0, false, false, false);
```

**防御性编程建议**：

- 调用 asc_mmad 前断言 `offset + m*n <= L0C_SIZE_FLOATS`（32768）
- 对 L0C 指针偏移做范围检查 `[0, L0C_SIZE - M*N]`

## 变体索引

| 变体 | C_BAD_OFFSET | M×N | 末址 | 越界量 |
|------|-------------|-----|------|--------|
| 基准（本用例） | 32640 | 256 | 32896 | +128 floats |
| 边界变体 | 32768 | 256 | 33024 | +256 floats |
| loop 累积变体 | i×256 (i=0..159) | 256 | 40960 | 见 cube_fixp_buffer_overflow |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：L0C 写址安全 | offset + m×n ≤ 32768 | fixed 版（0+256=256） |
| EC2：L0C 单次写越界 | offset + m×n > 32768（单次） | error 版（32640+256=32896） |
| EC3：L0C loop 累积越界 | Σ(offset_i + m×n) 跨边界（多次） | cube_fixp_buffer_overflow |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| cube_fixp_buffer_overflow (L13) | 同族 L13，loop 累积路径（本用例为单次大偏移路径） |
| cube_l0a_wraparound (L15) | 同族 cube core，L0A 回转 |
| cube_l0b_wraparound (L14) | 同族 cube core，L0B 回转 |

## 检测执行指引

- **适用匹配模式**：值域判断（`cube_l0c_overflow-RULE-01`）；diff_pattern（`cube_l0c_overflow-RULE-02`）。
- **命中判定**：按 RULE-02 定位 C_BAD_OFFSET 指针偏移注入，按 RULE-01 计算单次写入的末址并与本文的 L0C 容量比较；保留与 loop 累积路径的区别。保留以下取值、计算和日志核对步骤：

  1. 读取 error/main.asc 第 9 行 `C_BAD_OFFSET = 32640`，第 20 行 `cBad = cL0c + C_BAD_OFFSET`。
  2. 计算 32640 + M×N(256) = 32896 > L0C 32768 floats → 越界 128 floats。
  3. fixed 版第 20 行 `cBad = cL0c`（无偏移）→ 0+256=256 ≤ 32768 → 安全。
  4. error 版 stderr 应含 EZ9999 + retCode=0x26 + aic error mask=0x6500020bd00028c。
- **修复映射**：
  - `cube_l0c_overflow-RULE-01`、`cube_l0c_overflow-RULE-02` → 移除 `C_BAD_OFFSET` 注入，使用 `__cc__ float* cBad = cL0c`；重新核对 `offset + M×N <= 32768`，种子修复写址为 `[0, 256)`。
