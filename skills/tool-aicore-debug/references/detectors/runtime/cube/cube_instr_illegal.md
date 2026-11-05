# cube_instr_illegal — CUBE MMAD 指令操作数地址非对齐

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | cube_instr_illegal |
| 硬件事件编号 | L9 |
| 异常类型 | CUBE MMAD 指令操作数地址非对齐 |
| 错误码 | errcode:TBD（L9 cube core 路径，验证阶段回填） |
| 类别 | cube |
| 源文件 | error/main.asc:18（INJECT 字面量 true）；mmad_addr_injector.h（注入算术） |
| 报错模式 | EZ9999 + retCode=0x26 [aicore exception] + cube error info 非零 |
| 内存区域 | L0C（`__cc__` float* c_matrix 操作数） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 字面量切换（INJECT true→false）+ return 1→0 |
| behavior_note | 多文件 tiling 流水线；第 3 tile 注入 c 偏移 +1 元素（+4B）破坏 L0C 64B/16 元素对齐 |

## 触发条件

**违规模式**：GEMM tiling 流水线中，第 3 个 tile（tileIdx=2）经 `MmadAddrInjector::CalcCOffsetForTile(2)` 返回 `C_OFFSET_INJECT=1`，使 `asc_mmad(cL0c + 2*256 + 1, ...)` 的 c 操作数地址偏移 +1 float 元素（+4 字节），破坏 L0C 物理 64B/16 元素对齐约束 → MMAD 指令操作数地址對齊錯誤 → L9。INJECT=true 时构造 `GemmKernel op(true)`。

**安全模式**：`GemmKernel op(false)`，所有 tile cOff=0，`cL0c + {0, 256, 768}` 均 64B 对齐，正常计算流水线。

## 检测规则

### cube_instr_illegal-RULE-01：MMAD c 操作数地址非 64B 对齐

- 【条件】`asc_mmad` 的 c_matrix 指针（`__cc__` float*）地址偏移非 16 元素（64B）整数倍
- 【API】`asc_mmad(cL0c + tileIdx * C_TILE_OFF + cOff, aL0a, bL0b, ...)`；cOff 由 `MmadAddrInjector::CalcCOffsetForTile(tileIdx)` 返回
- 【约束】L0C 物理 64B/16 元素对齐；cOff=1 时写址 `cL0c + 513`（float），偏移 513×4=2052B，2052 mod 64 = 4 ≠ 0 → 违反对齐约束
- 【匹配模式】值域判断

### cube_instr_illegal-RULE-02：注入字面量（INJECT=true）路由

- 【条件】kernel 入口 `GemmKernel op(INJECT)` 中 INJECT 字面量为 true
- 【API】`GemmKernel op(true);`
- 【约束】INJECT=true → inject_ 成员为 true → 第 3 tile 注入 c 偏移
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：将 INJECT 字面量从 true 改为 false，使所有 tile 返回 cOff=0。

```cpp
// 修复前（error）
GemmKernel op(true);

// 修复后（fixed）
GemmKernel op(false);
```

**防御性编程建议**：

- 在 `asc_mmad` 调用前断言 c_matrix 地址满足 `((uintptr_t)cPtr - (uintptr_t)cL0c_base) % 64 == 0`
- 对 tile 偏移量做编译期静态断言：`static_assert((C_TILE_OFF * sizeof(float)) % 64 == 0)`

## 变体索引

| 变体 | 偏移量 | 触发 tile | 机制 |
|------|--------|----------|------|
| 基准（本用例） | cOff=1（+4B） | tileIdx=2 | +1 元素破坏 64B 对齐 |
| 大偏移变体 | cOff=2（+8B） | tileIdx=2 | +2 元素破坏 64B 对齐 |
| 多 tile 注入 | cOff=1 | tileIdx=1,2,3 | 多 tile 非对齐 |

## 等价类划分

| 等价类 | 描述 | 本用例 |
|--------|------|--------|
| EC1：c 操作数 64B 对齐 | (偏移×sizeof(float)) mod 64 == 0 | fixed 版 |
| EC2：c 操作数非 64B 对齐 | (偏移×sizeof(float)) mod 64 != 0 | error 版（cOff=1） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| cube_l0c_overflow (L13) | 同族 cube core 路径，L13 为 L0C 容量越界，本用例为地址对齐（不越容量） |
| cube_l0a_wraparound (L15) | 同族 cube core，L0A 写址回转 |
| cube_l0b_wraparound (L14) | 同族 cube core，L0B 写址回转 |

## 检测执行指引

- **适用匹配模式**：值域判断（`cube_instr_illegal-RULE-01`）；diff_pattern（`cube_instr_illegal-RULE-02`）。
- **命中判定**：按 RULE-02 定位 INJECT 字面量差异，再追踪到实际 cOff，按 RULE-01 计算 c 操作数是否满足本文的 64B 对齐约束；INJECT 路由证据需结合偏移计算解释。保留以下跨文件追踪和日志核对步骤：

  1. 读取 error/main.asc 第 18 行确认 `GemmKernel op(true)`（INJECT=true）。
  2. 追踪 inject_ 成员 → `MmadAddrInjector::CalcCOffsetForTile(2)` 返回 1。
  3. 计算 c 操作数地址：`(2*256 + 1) * 4 = 2052` 字节，`2052 % 64 = 4 ≠ 0` → 确认非对齐。
  4. fixed 版 `GemmKernel op(false)` → cOff 全为 0 → 对齐安全。
  5. error 版 stderr 应含 EZ9999 + retCode=0x26 + cube error info 非零；fixed 版无 EZ9999。
- **修复映射**：
  - `cube_instr_illegal-RULE-01` → 消除 cOff=1 注入，使 c_matrix 的实际偏移满足本文的 64B/16 元素对齐约束。
  - `cube_instr_illegal-RULE-02` → 将种子入口 `GemmKernel op(true)` 改为 `GemmKernel op(false)`，使所有 tile 返回 cOff=0。
