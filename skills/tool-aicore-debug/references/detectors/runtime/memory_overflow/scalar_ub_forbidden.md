# scalar_ub_forbidden — SCALAR 读写访问禁止 UB 空间（data_exception，来自 ccu）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | scalar_ub_forbidden |
| 硬件事件编号 | L148 |
| 异常类型 | SCALAR 读写访问禁止 UB 空间（data_exception，来自 ccu） |
| 错误码 | errcode:(0, 0x4000, 0)，errorStr: "CCU instruction address check error."，retCode=0x31，subErrType:4，runtime result=507035 |
| 类别 | memory_overflow |
| 源文件 | error/main.asc:20-21（`volatile __ubuf__ float* underflowPtr = buf + UNDERFLOW_OFFSET; *underflowPtr = (float)1.0`） |
| 报错模式 | EZ9999 + retCode=0x31 + errcode:(0, 0x4000, 0) "CCU instruction address check error." |
| 内存区域 | UB 下溢区域（buf + UNDERFLOW_OFFSET=-4096 元素=-16KB，UB 基址下方） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | SCALAR volatile store UB 下溢注入 + OOB 兜底（asc_copy_gm2ub_sync buf+65536）→ 移除注入 + return 1→0 |
| behavior_note | SCALAR volatile store 到 UB 下溢区域（buf-4096），CCU 指令级地址检查触发 data_exception；errcode novel (0,0x4000,0) ≠ L147 VEC ≠ MPU 非法；UB 下溢走 CCU 检查，UB 上界越界走 MPU 检查 |

## 触发条件

> **容量适用范围**：本文容量数值记录历史种子环境，不代表所有芯片或 tensor 的可访问容量。当前输入须先确认架构、API 重载及实际分配边界；未知时标记 `runtime_required=true`。统一单位与计分口径见 `references/constraints/semantic_contract.md`。

**违规模式**：通过 SCALAR 指令（volatile 单元素 store，强制 SCALAR codegen）访问 UB 下溢区域（`underflowPtr = buf + UNDERFLOW_OFFSET`，UNDERFLOW_OFFSET=-4096 元素=-16KB，UB 基址下方），使 SCALAR 管线触发 CCU 指令级地址检查错误。OOB 兜底置于主路径之后（`asc_copy_gm2ub_sync(buf+65536, ...)`），仅主路径静默时执行。

**安全模式**：移除 underflowPtr 声明 + UNDERFLOW_OFFSET/OOB_OFFSET 常量 + OOB 兜底，保留正常计算路径（gm2ub→add→ub2gm）。

**关键发现**：UB 上界越界（正偏移 ≥49152）→ MPU 内存级地址检查 → MPU 地址非法；UB 下溢区域（负偏移）及合法区域（偏移 0）→ CCU 指令级地址检查 → CCU instruction address check error。

## 检测规则

### scalar_ub_forbidden-RULE-01：SCALAR volatile store UB 下溢（主路径）

- 【条件】SCALAR 指令 volatile store 到 UB 下溢区域（负偏移），CCU 指令级地址检查
- 【API】`volatile __ubuf__ float* underflowPtr = (volatile __ubuf__ float*)(buf + UNDERFLOW_OFFSET); *underflowPtr = (float)1.0;`（error 第 20-21 行）
- 【约束】UNDERFLOW_OFFSET=-4096 元素=-16KB，UB 基址下方禁止区域 → CCU error (0, 0x4000, 0)
- 【匹配模式】diff_pattern

### scalar_ub_forbidden-RULE-02：OOB 兜底（L114，主路径触发后不执行）

- 【条件】`asc_copy_gm2ub_sync(buf+65536, ...)` MTE DMA 写越界 UB
- 【API】`__ubuf__ float* oobPtr = buf + OOB_OFFSET; asc_copy_gm2ub_sync((__ubuf__ void*)oobPtr, ...);`（error 第 23-24 行）
- 【约束】OOB_OFFSET=65536 偏移 256KB ≫ UB 声明 1KB → L114 scalar addr error；主路径先触发 CCU error → OOB 不执行
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：移除 SCALAR store UB 下溢注入 + OOB 兜底，保留正常计算路径。

```cpp
// 修复前（error）
constexpr int32_t UNDERFLOW_OFFSET = -4096;
constexpr uint32_t OOB_OFFSET = 65536;
volatile __ubuf__ float* underflowPtr = (volatile __ubuf__ float*)(buf + UNDERFLOW_OFFSET);
*underflowPtr = (float)1.0;
__ubuf__ float* oobPtr = buf + OOB_OFFSET;
asc_copy_gm2ub_sync((__ubuf__ void*)oobPtr, (__gm__ void*)x, TILE_LENGTH * sizeof(float));

// 修复后（fixed）
// underflowPtr / OOB_OFFSET 注入行全部移除，保留正常计算路径
asc_copy_gm2ub_sync((__ubuf__ void*)buf, (__gm__ void*)x, TILE_LENGTH * sizeof(float));
```

**防御性编程建议**：

- 禁止 SCALAR store 到 UB 基址下方（负偏移）或声明范围外
- UB 指针运算前断言偏移 ≥ 0 且 < 声明大小

## 变体索引

| 变体 | 访问方式 | offset（元素） | 结果 |
|------|---------|---------------|------|
| 1 | SCALAR store 正偏移 | 49152 (192KB) | MPU 地址非法 (0, 0x200000000000000, 0) — 未命中 |
| 2 | SCALAR load 正偏移 | 49152 | MPU 地址非法 — 未命中 |
| 4a | SCALAR store offset 0 | 0 | CCU error (0, 0x4000, 0) — 合法地址概念不符 |
| 5（基准） | SCALAR store 负偏移 | -4096 (-16KB) | CCU error (0, 0x4000, 0) — 命中采纳 |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：SCALAR UB 访问安全 | 合法区域 + 正常计算 | fixed 版 |
| EC2：UB 上界越界 | 正偏移 ≥49152 SCALAR store | MPU 地址非法（变体 1/2） |
| EC3：UB 下溢 | 负偏移 SCALAR store | CCU error (0, 0x4000, 0) — error 版（变体 5） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| vec_ub_forbidden_access (L147) | 同族 UB forbidden，VEC 计算路径（errcode 第一字段不同：0 vs 0x4000000000000000） |
| scalar_addr_error (10) | 同族 UB 访问，OOB 兜底机制复用（asc_copy_gm2ub_sync buf+65536） |
| dvg_stack_overflow / illegal_instr | 同为 VEC + buf+65536 OOB（MPU 非法），本用例主路径为 SCALAR store UB 下溢 |

## 检测执行指引

- **适用匹配模式**：diff_pattern（RULE-01、RULE-02）。
- **命中判定**：按下列步骤核对 RULE-01 的 SCALAR store UB 下溢；RULE-02 还需记录 error/main.asc 第 23-24 行 `buf + 65536` 的 MTE DMA 写越界兜底。主路径先触发 CCU error 时，兜底不执行，不将两者视为同时发生的运行时异常。
  1. 读取 error/main.asc 第 10 行 `UNDERFLOW_OFFSET = -4096`，第 20-21 行 `underflowPtr = buf + UNDERFLOW_OFFSET; *underflowPtr = (float)1.0`（SCALAR store UB 下溢）。
  2. 判定 UNDERFLOW_OFFSET=-4096 元素=-16KB → UB 基址下方禁止区域 → CCU 指令级地址检查。
  3. fixed 版移除 underflowPtr + OOB 注入，保留正常计算路径（gm2ub→add→ub2gm）→ 安全。
  4. error 版 stderr 应含 EZ9999 + retCode=0x31 + errcode:(0, 0x4000, 0) "CCU instruction address check error."。
- **修复映射**：RULE-01 → 移除 underflowPtr 构造和 SCALAR store 下溢注入；RULE-02 → 移除 OOB 指针及越界搬运，保留合法 `gm2ub→add→ub2gm` 计算路径。
