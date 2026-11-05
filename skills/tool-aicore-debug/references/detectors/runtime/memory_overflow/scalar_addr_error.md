# scalar_addr_error — UB 地址越界（经 MTE DMA 通道）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | scalar_addr_error |
| 硬件事件编号 | 10 |
| 异常类型 | UB 地址越界（经 MTE DMA 通道） |
| 错误码 | errcode:(0, 0x10000, 0) 或 (0x200000000, 0, 0)（视路由，验证阶段确认） |
| 类别 | memory_overflow |
| 源文件 | error/main.asc:18（`__ubuf__ float* badPtr = xLocal + OOB_OFFSET`）+ :20（`asc_copy_gm2ub_sync` 写越界 UB） |
| 报错模式 | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| 内存区域 | UB（`__ubuf__` float*，声明 256 元素=1KB，OOB_OFFSET=65536 即偏移 256KB） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | UB 指针偏移常量注入（OOB_OFFSET=65536→0）+ return 1→0 |
| behavior_note | badPtr=xLocal+65536 偏移 256KB 远超 UB 声明 1KB，asc_copy_gm2ub_sync 向越界 UB 地址写入触发 UB OOB 异常 |

## 触发条件

**违规模式**：`__ubuf__ float* badPtr = xLocal + OOB_OFFSET`（OOB_OFFSET=65536，偏移 256KB）远超声明 256 元素的 UB buffer（1KB），`asc_copy_gm2ub_sync((__ubuf__ void*)badPtr, ...)` 向越界 UB 地址写入 → UB OOB 异常。实现范式为经 MTE DMA 通道（`asc_copy_gm2ub_sync` + 越界 `__ubuf__` 指针），非狭义 scalar ld/st 指令。

**安全模式**：`__ubuf__ float* badPtr = xLocal;`（无偏移），asc_copy_gm2ub_sync 写入 UB 声明范围内。

## 检测规则

### scalar_addr_error-RULE-01：__ubuf__ 指针 OOB 偏移注入

- 【条件】`__ubuf__ float*` 指针通过 `+ OOB_OFFSET` 构造，OOB_OFFSET 远超 UB 声明元素数
- 【API】`__ubuf__ float* badPtr = xLocal + OOB_OFFSET;`（error 第 18 行）vs `badPtr = xLocal;`（fixed 第 18 行）
- 【约束】OOB_OFFSET=65536 元素=256KB，UB 声明 TILE_LENGTH=256 元素=1KB → 65536 ≫ 256，越界 65280 元素
- 【匹配模式】diff_pattern

### scalar_addr_error-RULE-02：asc_copy_gm2ub_sync 目标 UB 指针越界

- 【条件】`asc_copy_gm2ub_sync` 的 dst `__ubuf__` 指针 = xLocal + 65536，偏移超出 UB 声明范围
- 【API】`asc_copy_gm2ub_sync((__ubuf__ void*)badPtr, (__gm__ void*)x, TILE_LENGTH * sizeof(float));`（error 第 20 行）
- 【约束】dst badPtr=xLocal+65536，写入 256 元素到 [65536, 65792)，全部在 UB 声明 [0,256) 之外
- 【匹配模式】值域判断

## 修复方案

**标准修复模式**：移除 UB 指针偏移，使用合法基址。

```cpp
// 修复前（error）
constexpr uint32_t OOB_OFFSET = 65536;
__ubuf__ float* badPtr = xLocal + OOB_OFFSET;
asc_copy_gm2ub_sync((__ubuf__ void*)badPtr, (__gm__ void*)x, TILE_LENGTH * sizeof(float));

// 修复后（fixed）
__ubuf__ float* badPtr = xLocal;
asc_copy_gm2ub_sync((__ubuf__ void*)badPtr, (__gm__ void*)x, TILE_LENGTH * sizeof(float));
```

**防御性编程建议**：

- 调用 asc_copy_gm2ub_sync 前断言 dst 指针偏移 + count ≤ UB 声明大小
- 对 `__ubuf__` 指针偏移做范围检查 `[0, UB_SIZE - count]`

## 变体索引

| 变体 | OOB_OFFSET | 越界量 | 结果 |
|------|-----------|--------|------|
| 基准（本用例） | 65536（256KB） | +65280 元素 | UB OOB（主路径） |
| 地址非对齐变体 | raw+2（byte 级） | misalign | 回退方案（ADDR_MISALIGN） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：UB 访问安全 | 指针偏移 + count ≤ 声明 256 元素 | fixed 版（0+256=256） |
| EC2：UB OOB | 指针偏移远超声明范围 | error 版（65536≫256） |
| EC3：地址非对齐 | 指针 byte 级偏移 | 回退变体 |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| scalar_gm_addr_oob (L107) | 同族地址越界，GM>48bit 路径 |
| scalar_ub_forbidden (L148) | 同族 UB 访问异常，SCALAR store 下溢路径 |
| vec_ub_forbidden_access (L147) | 同族 UB 访问异常，VEC 计算路径 CCU 检查 |

## 检测执行指引

- **适用匹配模式**：diff_pattern（RULE-01）、值域判断（RULE-02）。
- **命中判定**：按下列步骤核对 RULE-01 的 UB 指针偏移差异；RULE-02 还需确认 error/main.asc 第 20 行 `asc_copy_gm2ub_sync` 以 badPtr 为 dst，写入区间 `[65536, 65792)` 全部落在声明范围 `[0, 256)` 之外。
  1. 读取 error/main.asc 第 9 行 `OOB_OFFSET = 65536`，第 18 行 `badPtr = xLocal + OOB_OFFSET`。
  2. 计算 65536 > 声明 256 元素 → 越界 65280 元素（偏移 256KB ≫ UB 声明 1KB）。
  3. fixed 版第 18 行 `badPtr = xLocal`（无偏移）→ 0+256=256 ≤ 256 → 安全。
  4. error 版 stderr 应含 EZ9999（验证阶段确认 retCode/errcode）。
- **修复映射**：RULE-01、RULE-02 → 移除 UB 偏移，使用 `badPtr = xLocal`，并验证搬运的偏移加 count 不超过 UB 声明大小。
