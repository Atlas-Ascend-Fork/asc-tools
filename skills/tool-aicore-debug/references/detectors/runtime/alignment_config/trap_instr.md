# trap_instr — trap 指令异常（OOB 兜底 ADDR_MISALIGN）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | trap_instr |
| 硬件事件编号 | L98 |
| 异常类型 | trap instr error |
| 错误码 | errcode:(TBD, TBD, 0)（L98 主路径无先例，OOB 降级 ADDR_MISALIGN 族） |
| 类别 | alignment_config |
| 源文件 | error/main.asc:17（`asm volatile("trap")`）+ :19-20（`__ubuf__ half* oobPtr = buf + OOB_OFFSET; *oobPtr = (half)1.0f`） |
| 报错模式 | EZ9999（验证阶段回填 retCode/exit_code/errcode/subErrType/vec error info） |
| 内存区域 | UB（buf+OOB_OFFSET=65536，偏移 128KiB 远超声明 512B） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | inline asm trap 注入 + OOB 兜底 UB 写越界 → 合法 store + return 1→0 |
| behavior_note | 主路径 asm volatile("trap") 下发 trap 指令（探索性，trap 无 C API）；OOB 兜底 buf+65536 ≫ UB 声明 → 越过声明边界，实际硬件上报需验证（历史其他种子记录了 OOB 报错，不能替代本目标验证） |

## 触发条件

**违规模式**：主路径 `asm volatile("trap")` 下发 trap 指令（探索性，trap 无 C API，编码可能不被编译器接受）。OOB 兜底 `__ubuf__ half* oobPtr = buf + OOB_OFFSET; *oobPtr = (half)1.0f;` — buf+65536 元素远超 UB 声明 512B。只有整个 kernel 已编译且执行到该写入时，兜底才可能提供地址异常证据；若 asm 编译失败，则没有设备执行，不能宣称发生 EZ9999。兜底报错也不证明 trap 目标已验证。

**安全模式**：移除 `asm volatile("trap")` 行 + OOB 兜底行，改 `buf[0] = (half)1.0f;`（合法 store）。

**inline asm 编码变体链**（编码阶段按序尝试，首个编译通过的采纳）：

1. 首选：`asm volatile("trap");`（助记符形式，本用例采用）
2. `asm volatile("trap.nz");`（带条件码变体）
3. `asm volatile(".word 0x0000007d");`（直接机器码）
4. 编译全失败 → 仅保留 OOB 兜底，README 标注降级

## 检测规则

### trap_instr-RULE-01：inline asm trap 指令注入（主路径）

- 【条件】通过 `asm volatile("trap")` 下发 trap 指令（探索性，trap 无 C API）
- 【API】`asm volatile("trap");`（error 第 17 行）vs 移除（fixed）
- 【约束】trap 指令无 C API 发射方式，inline asm 为唯一路径；编译器是否接受待验证
- 【匹配模式】diff_pattern

### trap_instr-RULE-02：OOB 兜底 UB 写越界（ADDR_MISALIGN）

- 【条件】`__ubuf__ half* oobPtr = buf + OOB_OFFSET`（OOB_OFFSET=65536），`*oobPtr = (half)1.0f`
- 【API】`__ubuf__ half* oobPtr = buf + OOB_OFFSET; *oobPtr = (half)1.0f;`（error 第 19-20 行）
- 【约束】oobPtr=buf+65536 偏移 128KiB ≫ UB 声明 512B（TILE_LENGTH=256 half）→ 越过声明边界，实际硬件上报需验证
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：移除 trap 指令注入 + OOB 兜底，改合法 store。

```cpp
// 修复前（error）
asm volatile("trap");
__ubuf__ half* oobPtr = buf + OOB_OFFSET;
*oobPtr = (half)1.0f;

// 修复后（fixed）
buf[0] = (half)1.0f;
```

**防御性编程建议**：

- 禁止在 kernel 中注入 trap 指令（仅调试用途）
- 禁止 store 到 UB 声明范围外

## 变体索引

| 变体 | asm 编码 | 编译结果 | 结果 |
|------|---------|---------|------|
| 1（基准） | `asm volatile("trap")` | 待验证 | L98 trap 或 OOB 兜底 ADDR_MISALIGN |
| 2 | `asm volatile("trap.nz")` | 待验证 | 同上 |
| 3 | `asm volatile(".word 0x0000007d")` | 待验证 | 同上 |
| 4 | 编译失败→仅 OOB | ✅ | ADDR_MISALIGN 降级 |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：trap 安全 | 无 trap 指令 + 合法 store | fixed 版 |
| EC2：trap 注入 | asm volatile("trap") | error 版主路径（待验证） |
| EC3：OOB 兜底 | buf+65536 store 越界 | error 版兜底（ADDR_MISALIGN） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| illegal_instr (L118) | 同族指令异常，inline asm `.int` 注入路径（OOB 兜底机制复用） |
| scalar_int_neg_sqrt (6) | OOB 兜底机制验证来源（其他类型的 OOB 记录，须按本种子 half 元素大小重新核对） |
| scalar_addr_error (10) | 同族 OOB 兜底（UB 写越界） |

## 检测执行指引

- **适用匹配模式**：diff_pattern（RULE-01/RULE-02）
- **命中判定**：RULE-01 对应 trap 指令注入，编译器是否接受待验证；RULE-02 对应 OOB 兜底，需区分 L98 trap 与 ADDR_MISALIGN 降级路径。
  - 读取 error/main.asc 第 10 行 `OOB_OFFSET = 65536`，第 17 行 `asm volatile("trap")`，第 19-20 行 `oobPtr = buf + OOB_OFFSET; *oobPtr = (half)1.0f`。
  - 判定主路径 trap 指令（待验证编译器接受）；OOB 兜底 buf+65536 偏移 128KiB ≫ UB 声明 512B → 越过声明边界，实际硬件上报需验证。
  - error 版 stderr 应含 EZ9999（验证阶段确认 errcode，L98 trap 或 OOB 降级 ADDR_MISALIGN）。
- **修复映射**：
  - trap_instr-RULE-01 → 移除 `asm volatile("trap")` 指令注入。
  - trap_instr-RULE-02 → 移除 OOB 兜底，种子 fixed 第 16 行改为 `buf[0] = (half)1.0f`（合法 store）。
