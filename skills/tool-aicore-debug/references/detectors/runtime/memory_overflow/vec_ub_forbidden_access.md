# vec_ub_forbidden_access — VEC 读写禁止 UB 空间（data_exception）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | vec_ub_forbidden_access |
| 硬件事件编号 | L147 |
| 异常类型 | VEC 读写禁止 UB 空间（data_exception） |
| 错误码 | errcode:(0x4000000000000000, 0x4000, 0)，errorStr: "VEC instruction error: the ub address out of bounds. CCU instruction address check error."，retCode=0x31，subErrType:4 |
| 类别 | memory_overflow |
| 源文件 | error/main.asc:20（`__ubuf__ float* forbiddenPtr = buf + FORBIDDEN_OFFSET`）+ :24（`asc_add_sync(zLocal, forbiddenPtr, yLocal, TILE_LENGTH)`） |
| 报错模式 | EZ9999 + retCode=0x31 + errcode:(0x4000000000000000, 0x4000, 0) "VEC instruction error: the ub address out of bounds. CCU instruction address check error." |
| 内存区域 | UB 越界区域（buf + FORBIDDEN_OFFSET=49152 元素=192KB） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | VEC 计算 src1 forbidden 指针注入 + OOB 兜底（buf+65536 直接写）→ 合法 src1 + return 1→0 |
| behavior_note | VEC 计算路径 asc_add_sync src1 指向 UB 越界区域 buf+49152（192KB），CCU 指令级地址检查检测到 UB 地址越界触发 VEC instruction error；直接指针写走 MPU 检查，VEC 计算路径走 CCU 检查 |

## 触发条件

> **容量适用范围**：本文容量数值记录历史种子环境，不代表所有芯片或 tensor 的可访问容量。当前输入须先确认架构、API 重载及实际分配边界；未知时标记 `runtime_required=true`。统一单位与计分口径见 `references/constraints/semantic_contract.md`。

**违规模式**：通过 VEC 计算路径 `asc_add_sync(zLocal, forbiddenPtr, yLocal, TILE_LENGTH)` 将 src1 指向 UB 越界区域（`forbiddenPtr = buf + FORBIDDEN_OFFSET`，FORBIDDEN_OFFSET=49152 元素=192KB），VEC 计算单元（CCU）在指令执行时进行地址检查，检测到 UB 地址越界，触发 VEC instruction error（data_exception 类型，L147）。OOB 兜底置于计算路径之后（`*oobPtr = (float)1.0`，buf+65536 直接 VEC 写越界 → MPU 地址非法），仅主路径未触发时执行。

**关键发现**：直接指针写访问（`*ptr = v`）走 load/store 单元 → MPU 内存级地址检查，越界触发 MPU 地址非法；VEC 计算路径（`asc_add_sync`）src 指针走 VEC 计算单元 → CCU 指令级地址检查，越界触发 VEC instruction error。UB 边界约 190-192KB。

**安全模式**：移除 forbiddenPtr 声明 + FORBIDDEN_OFFSET/OOB_OFFSET 常量 + OOB 兜底，asc_add_sync 的 src1 从 forbiddenPtr 改为 buf。

## 检测规则

### vec_ub_forbidden_access-RULE-01：asc_add_sync src1 指向 UB 越界区域（主路径 L147）

- 【条件】VEC 计算路径 `asc_add_sync` 的 src1 指针指向 UB 越界区域，CCU 指令级地址检查
- 【API】`asc_add_sync(zLocal, forbiddenPtr, yLocal, TILE_LENGTH);`（error 第 24 行）vs `asc_add_sync(zLocal, buf, yLocal, TILE_LENGTH);`（fixed 第 20 行）
- 【约束】forbiddenPtr=buf+49152 元素=192KB ≈ UB 边界 → CCU 地址检查 error (0x4000000000000000, 0x4000, 0)
- 【匹配模式】diff_pattern

### vec_ub_forbidden_access-RULE-02：OOB 兜底（MPU 地址非法，主路径触发后不执行）

- 【条件】`__ubuf__ float* oobPtr = buf + OOB_OFFSET`（OOB_OFFSET=65536），`*oobPtr = (float)1.0` 直接 VEC 写越界
- 【API】`__ubuf__ float* oobPtr = buf + OOB_OFFSET; *oobPtr = (float)1.0;`（error 第 27-28 行）
- 【约束】oobPtr=buf+65536 偏移 256KB → 直接指针写 MPU 地址非法 (0, 0x200000000000000, 0)；主路径先触发 CCU error → OOB 不执行
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：asc_add_sync 的 src1 从 forbidden 指针改为合法 UB 地址，移除 OOB 兜底。

```cpp
// 修复前（error）
constexpr uint32_t FORBIDDEN_OFFSET = 49152;
__ubuf__ float* forbiddenPtr = buf + FORBIDDEN_OFFSET;
asc_add_sync(zLocal, forbiddenPtr, yLocal, TILE_LENGTH);   // src1 指向 UB 越界
__ubuf__ float* oobPtr = buf + OOB_OFFSET;
*oobPtr = (float)1.0;                                      // OOB 兜底

// 修复后（fixed）
asc_add_sync(zLocal, buf, yLocal, TILE_LENGTH);            // src1 合法
// forbiddenPtr / OOB 注入行全部移除
```

**防御性编程建议**：

- VEC 计算 API 的 src/dst 指针应指向 UB 声明范围内
- 禁止将 `buf + 大偏移` 传入 VEC 计算 API 作为 src/dst
- UB 指针运算前断言偏移 < UB 边界（~190KB）

## 变体索引

| 变体 | 访问方式 | offset（元素） | 结果 |
|------|---------|---------------|------|
| 1 | 直接指针写 | 49152 (192KB) | MPU 地址非法 (0, 0x200000000000000, 0) — 未命中 |
| 5 | 直接指针读 | 49152 | MPU 地址非法 — 未命中 |
| 6 | 负偏移直接写 | -4096 (-16KB) | MPU 地址非法 — 未命中 |
| 7（基准） | asc_add_sync 计算 | 49152 (192KB) | CCU error (0x4000000000000000, 0x4000, 0) — 命中采纳 |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：VEC UB 访问安全 | 合法 src/dst + 正常计算 | fixed 版 |
| EC2：直接指针写越界 | 直接指针 store/load 越界 | MPU 地址非法（变体 1/5/6） |
| EC3：VEC 计算路径越界 | VEC 计算 API src/dst 越界 | CCU error（变体 7） — error 版 |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| scalar_ub_forbidden (L148) | 同族 UB forbidden，SCALAR 路径（errcode 第一字段不同：0x4000000000000000 vs 0） |
| dvg_stack_overflow | 同为 VEC + buf+offset UB 访问，stack 溢出路径 |
| ub_addr_misalign | 同族 UB 访问异常，UB 非对齐（byte 级偏移）路径 |
| illegal_instr | 同为 VEC + OOB 兜底（MPU 非法），主路径不同 |

## 检测执行指引

- **适用匹配模式**：diff_pattern（RULE-01、RULE-02）。
- **命中判定**：按下列步骤核对 RULE-01 的 asc_add_sync src1 越界；RULE-02 还需记录 error/main.asc 第 27-28 行 `buf + 65536` 的直接写越界兜底。主路径先触发 CCU error 时，兜底不执行，不将兜底的 MPU 错误码作为主路径已触发的证据。
  1. 读取 error/main.asc 第 10 行 `FORBIDDEN_OFFSET = 49152`，第 20 行 `forbiddenPtr = buf + FORBIDDEN_OFFSET`，第 24 行 `asc_add_sync(zLocal, forbiddenPtr, yLocal, TILE_LENGTH)`。
  2. 判定 forbiddenPtr=buf+49152 元素=192KB ≈ UB 边界 → VEC 计算路径 CCU 地址检查。
  3. fixed 版第 20 行 `asc_add_sync(zLocal, buf, yLocal, TILE_LENGTH)`（合法 src1）→ 安全。
  4. error 版 stderr 应含 EZ9999 + retCode=0x31 + errcode:(0x4000000000000000, 0x4000, 0) "VEC instruction error: the ub address out of bounds. CCU instruction address check error."。
- **修复映射**：RULE-01 → 将 asc_add_sync 的 src1 从 forbiddenPtr 改为合法 buf，移除 forbiddenPtr 构造；RULE-02 → 移除 OOB 指针及直接写越界注入。
