# cube_l0b_wraparound — CUBE L0B 卷绕越界

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | cube_l0b_wraparound |
| 硬件事件编号 | L14 |
| 异常类型 | CUBE L0B 卷绕越界 |
| 错误码 | errcode:TBD（L14 与 L13 同族 cube core 路径） |
| 类别 | cube |
| 源文件 | error/main.asc:18（INJECT 字面量 true）；l0b_address_calc.h（注入算术） |
| 报错模式 | EZ9999 + retCode=0x26 [aicore exception] + cube error info 非零 |
| 内存区域 | L0B（`__cb__` half*，dav-2201 = 64KB = 32768 half） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 字面量切换（INJECT true→false）+ return 1→0 |
| behavior_note | 多文件 GEMM tiling 流水线；深层 tile 2 注入 {repeat=8, dstGap=8192} → 8×8192=128KB ≫ L0B 64KB |

## 触发条件

> **容量适用范围**：本文容量数值记录历史种子环境，不代表所有芯片或 tensor 的可访问容量。当前输入须先确认架构、API 重载及实际分配边界；未知时标记 `runtime_required=true`。统一单位与计分口径见 `references/constraints/semantic_contract.md`。

**违规模式**：GEMM tiling 流水线中第 3 个 tile（tileIdx=2）经 `L0bAddrCalc::CalcForTile(2)` 返回 `{repeat=8, dstGap=8192}`，执行 `asc_copy_l12l0b(bL0b, bL1, 0, 8, 0, 8192)`，8×8192=65536 half=128KB ≫ dav-2201 L0B 64KB 边界 → 地址回转 → L14。INJECT=true 时构造 `GemmKernel op(true)`。tile 0/1 正常（`{1, 0}`）。

**安全模式**：`GemmKernel op(false)`，所有 tile 返回 `{1, 0}`，正常 GEMM 流水线。

## 检测规则

### cube_l0b_wraparound-RULE-01：asc_copy_l12l0b dst_gap×repeat 超 L0B 容量（深层 tile 注入）

- 【条件】深层 tile（tileIdx=2）经 L0bAddrCalc 返回 {repeat=8, dstGap=8192}，使 asc_copy_l12l0b 写址超 L0B 64KB
- 【API】`asc_copy_l12l0b(bL0b, bL1, 0, p.repeat, 0, p.dstGap)`；p 由 `calc.CalcForTile(tileIdx)` 决定
- 【约束】L0B 容量 = 32768 half（dav-2201 = 64KB）；8×8192=65536 > 32768 → 越界回转
- 【匹配模式】值域判断

### cube_l0b_wraparound-RULE-02：注入字面量（INJECT=true）路由

- 【条件】kernel 入口 `GemmKernel op(INJECT)` 中 INJECT 为 true
- 【API】`GemmKernel op(true);`
- 【约束】INJECT=true → inject_ 成员为 true → 第 3 tile 注入 {8, 8192}
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：将 INJECT 字面量从 true 改为 false，使所有 tile 返回 {repeat=1, dstGap=0}。

```cpp
// 修复前（error）
GemmKernel op(true);

// 修复后（fixed）
GemmKernel op(false);
```

**防御性编程建议**：

- 在 L0bAddrCalc::CalcForTile 后断言 `repeat * dstGap * sizeof(half) <= L0B_SIZE_BYTES`（65536）
- 注入类应仅在测试构建启用，生产构建强制 inject_=false

## 变体索引

| 变体 | repeat | dst_gap | repeat×dst_gap | 越界量 |
|------|--------|---------|----------------|--------|
| 基准（本用例） | 8 | 8192 | 65536 half=128KB | +64KB |
| 小 repeat 变体 | 4 | 8192 | 32768 half=64KB | 恰到边界 |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：L0B 写址安全 | repeat×dst_gap×2 ≤ 65536 | fixed 版（1×0=0） |
| EC2：L0B 写址回转 | repeat×dst_gap×2 > 65536 | error 版 tile 2（8×8192） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| cube_l0a_wraparound (L15) | 同族同机制，L0A 写址回转 |
| cube_l0c_overflow (L13) | 同族，L0C 写址回转（单次大偏移路径） |
| cube_fixp_buffer_overflow (L13) | 同族 L13，loop 累积路径 |

## 检测执行指引

- **适用匹配模式**：值域判断（`cube_l0b_wraparound-RULE-01`）；diff_pattern（`cube_l0b_wraparound-RULE-02`）。
- **命中判定**：按 RULE-02 定位 INJECT 字面量差异，沿成员值追踪到 tile 2 的 repeat/dstGap，再按 RULE-01 核对 L0B 写址范围；同时保留 tile 0/1 返回 `{1, 0}` 的种子区别。保留以下跨文件追踪和日志核对步骤：

  1. 读取 error/main.asc 第 18 行 `GemmKernel op(true)` 确认 INJECT=true。
  2. 追踪 inject_ → `L0bAddrCalc::CalcForTile(2)` 返回 {8, 8192}。
  3. 计算 8×8192=65536 half=128KB > L0B 64KB → 回转。
  4. fixed 版 `GemmKernel op(false)` → 所有 tile 返回 {1, 0} → 安全。
  5. error 版 stderr 应含 EZ9999 + retCode=0x26 + cube error info 非零。
- **修复映射**：
  - `cube_l0b_wraparound-RULE-01` → 消除 tile 2 的 `{repeat=8, dstGap=8192}` 越界注入，恢复种子中的合法参数 `{1, 0}`。
  - `cube_l0b_wraparound-RULE-02` → 将 `GemmKernel op(true)` 改为 `GemmKernel op(false)`，使所有 tile 返回 `{repeat=1, dstGap=0}`。
