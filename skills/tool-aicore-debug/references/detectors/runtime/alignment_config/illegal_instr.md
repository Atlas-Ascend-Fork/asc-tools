# illegal_instr — 非法指令异常（OOB 兜底 ADDR_MISALIGN）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | illegal_instr |
| 硬件事件编号 | L118 |
| 异常类型 | illegal instruction |
| 错误码 | errcode:(TBD, TBD, 0)（L118 主路径无先例，OOB 降级 ADDR_MISALIGN 族） |
| 类别 | alignment_config |
| 源文件 | error/main.asc:17（`asm volatile(".int 0x00000000")`）+ :19-20（`__ubuf__ half* oobPtr = buf + OOB_OFFSET; *oobPtr = (half)1.0f`） |
| 报错模式 | EZ9999（验证阶段回填 retCode/exit_code/errcode/subErrType/ifu error info） |
| 内存区域 | UB（buf+OOB_OFFSET=65536，偏移 128KiB 远超声明 512B） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | inline asm `.int` 注入 + OOB 兜底 UB 写越界 → 合法 store + return 1→0 |
| behavior_note | 主路径 asm volatile(".int 0x00000000") 注入全零机器码（探索性，`.word` 被 bisheng 拒绝，改用 `.int`）；OOB 兜底 buf+65536 ≫ UB 声明 → 越过声明边界，实际硬件上报需验证（历史其他种子记录了 OOB 报错，不能替代本目标验证） |

## 触发条件

**违规模式**：主路径 `asm volatile(".int 0x00000000")` 注入全零机器码编码（探索性，`.word` 伪指令被 bisheng 编译器拒绝，改用语义等价的 `.int` 指令）。OOB 兜底 `__ubuf__ half* oobPtr = buf + OOB_OFFSET; *oobPtr = (half)1.0f;` — buf+65536（128KiB）≫ UB 声明 512B → 越过声明边界，实际硬件上报需验证。仅在 kernel 成功编译且执行到兜底写入时，该越界才可能产生地址异常；不能用兜底验证主路径，也不能保证特定硬件错误码。

**安全模式**：移除 `asm volatile(".int 0x00000000")` 行 + OOB 兜底行，改 `buf[0] = (half)1.0f;`（合法 store）。

**inline asm 编码变体链**（`.word` 全部编译失败，`.int` 被编译器接受）：

1. `asm volatile(".word 0x00000000")` — **编译失败**：bisheng 不支持 `.word` 伪指令
2. `asm volatile(".word 0xFFFFFFFF")` — **编译失败**：同上
3. `asm volatile(".int 0x00000000")` — ✅ 编译通过（`.int` 语义等价，发射 4 字节整数值）

## 检测规则

### illegal_instr-RULE-01：inline asm `.int` 非法机器码注入（主路径）

- 【条件】通过 `asm volatile(".int 0x00000000")` 注入全零机器码（非法指令编码）
- 【API】`asm volatile(".int 0x00000000");`（error 第 17 行）vs 移除（fixed）
- 【约束】全零机器码为非法指令编码（`.word` 被 bisheng 拒绝，`.int` 等价替代）；运行时是否触发非法指令异常待验证
- 【匹配模式】diff_pattern

### illegal_instr-RULE-02：OOB 兜底 UB 写越界（ADDR_MISALIGN）

- 【条件】`__ubuf__ half* oobPtr = buf + OOB_OFFSET`（OOB_OFFSET=65536），`*oobPtr = (half)1.0f`
- 【API】`__ubuf__ half* oobPtr = buf + OOB_OFFSET; *oobPtr = (half)1.0f;`（error 第 19-20 行）
- 【约束】oobPtr=buf+65536 偏移 128KiB ≫ UB 声明 512B（TILE_LENGTH=256 half）→ 越过声明边界，实际硬件上报需验证
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：移除非法指令注入 + OOB 兜底，改合法 store。

```cpp
// 修复前（error）
asm volatile(".int 0x00000000");
__ubuf__ half* oobPtr = buf + OOB_OFFSET;
*oobPtr = (half)1.0f;

// 修复后（fixed）
buf[0] = (half)1.0f;
```

**防御性编程建议**：

- 禁止在 kernel 中注入非法机器码（`.int`/`.word` 非法编码）
- 禁止 store 到 UB 声明范围外

## 变体索引

| 变体 | asm 编码 | 编译结果 | 结果 |
|------|---------|---------|------|
| 1 | `.word 0x00000000` | ✗ 失败（bisheng 不支持） | — |
| 2 | `.word 0xFFFFFFFF` | ✗ 失败 | — |
| 3（基准） | `.int 0x00000000` | ✅ 通过 | L118 illegal 或 OOB 兜底 ADDR_MISALIGN |
| 4 | `.int 0xFFFFFFFF` | ✅ 通过 | 全一编码（同族） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：指令安全 | 无非法注入 + 合法 store | fixed 版 |
| EC2：非法指令注入 | asm volatile(".int ...") | error 版主路径（待验证运行时） |
| EC3：OOB 兜底 | buf+65536 store 越界 | error 版兜底（ADDR_MISALIGN） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| trap_instr (L98) | 同族指令异常，inline asm trap 注入路径（OOB 兜底机制复用） |
| scalar_int_neg_sqrt (6) | OOB 兜底机制验证来源（其他类型的 OOB 记录，须按本种子 half 元素大小重新核对） |
| vec_ub_forbidden_access (L147) | 同为 VEC + OOB 兜底（MPU 非法），主路径不同 |

## 检测执行指引

- **适用匹配模式**：diff_pattern（RULE-01/RULE-02）
- **命中判定**：RULE-01 对应非法机器码注入，运行时是否触发非法指令异常待验证；RULE-02 对应 OOB 兜底，不能把兜底错误当作主路径已验证。
  - 读取 error/main.asc 第 10 行 `OOB_OFFSET = 65536`，第 17 行 `asm volatile(".int 0x00000000")`，第 19-20 行 `oobPtr = buf + OOB_OFFSET; *oobPtr = (half)1.0f`。
  - 判定主路径 `.int 0x00000000` 全零机器码（待验证运行时触发非法指令）；OOB 兜底 buf+65536 偏移 128KiB ≫ UB 声明 512B → 越过声明边界，实际硬件上报需验证。
  - error 版 stderr 应含 EZ9999（验证阶段确认 errcode，L118 illegal 或 OOB 降级 ADDR_MISALIGN + ifu error info）。
- **修复映射**：
  - illegal_instr-RULE-01 → 移除 `asm volatile(".int 0x00000000")` 非法机器码注入。
  - illegal_instr-RULE-02 → 移除 OOB 兜底，种子 fixed 第 16 行改为 `buf[0] = (half)1.0f`（合法 store）。
