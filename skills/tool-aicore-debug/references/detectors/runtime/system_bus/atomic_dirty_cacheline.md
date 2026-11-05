# atomic_dirty_cacheline — Atomic 访问 dirty cacheline

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | atomic_dirty_cacheline |
| 硬件事件编号 | L102 |
| 异常类型 | Atomic 访问 dirty cacheline |
| 需求目标 | L102 atomic 命中 normal store 修改的 dirty cacheline |
| 实际覆盖 | GM 标量 store + atomic add 同址探索 L102（主路径，dirty cacheline 依赖运行时状态）；OOB 兜底 UB 写越界致 ADDR_MISALIGN（scalar_int_neg_sqrt 已验证） |
| 错误码 | errcode:(TBD, TBD, 0)（L102 主路径无先例，OOB 降级 ADDR_MISALIGN 族） |
| 类别 | system_bus |
| 源文件 | error/main.asc:10（`OOB_OFFSET = 65536`）+ :18-23（dirty cacheline + atomic add 同址）+ :26-27（OOB 兜底） |
| 报错模式 | EZ9999（验证阶段补充 retCode/errcode/subErrType/vec error info 字段） |
| 内存区域 | GM（dirty cacheline 主路径）+ UB（OOB 兜底） |
| API层级 | TC-Low |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 结构变更（OOB_OFFSET=65536→0 + atomic 目标 z[0]→y[0] 结构隔离）+ return 1→0 |
| behavior_note | 主路径 GM 标量 store 创建 dirty cacheline + atomic add 同址探索 L102；OOB 兜底 buf+65536 >> UB 1KB 致 ADDR_MISALIGN |

## 触发条件

**违规模式（主路径）**：`((__gm__ volatile int32_t*)z)[0] = dirtyVal` 创建 dirty cacheline（SU 标量 store）→ `asc_set_atomic_add_int()` 切换 atomic 模式 → `asc_copy_ub2gm_sync(&z[0], val, 4)` atomic add 同址命中 dirty cacheline（L102 探索，dirty cacheline 依赖运行时状态）。

**违规模式（OOB 兜底）**：`__ubuf__ int32_t* oobPtr = buf + OOB_OFFSET; *oobPtr = dirtyVal;` — buf+65536 >> UB 声明 1KB → ADDR_MISALIGN（scalar_int_neg_sqrt 已验证必触发 EZ9999）。

**API 修正**：任务文件指定的 `asc_store_dev` 在 dav-2201 不可用（仅 3510）。改用 GM 标量解引用（SU 标量 store），与 scalar_int_div0_tclow 的 GM 读路径对称。

**安全模式**：`OOB_OFFSET = 0`（合法 UB 偏移）+ atomic 目标改为 `&y[0]`（结构隔离，atomic 不命中 z[0] dirty cacheline）。

## 检测规则

### atomic_dirty_cacheline-RULE-01：GM 标量 store + atomic add 同址命中 dirty cacheline

- 【条件】先对 GM 地址做标量 store（创建 dirty cacheline），再切换 atomic 模式对同址做 atomic add
- 【API】`((__gm__ volatile int32_t*)z)[0] = dirtyVal; asc_set_atomic_add_int(); asc_copy_ub2gm_sync((__gm__ void*)&z[0], (__ubuf__ void*)val, sizeof(int32_t));`
- 【约束】z[0] 先被标量 store 弄脏 → atomic add 命中 dirty cacheline → L102（dirty cacheline 依赖运行时状态）
- 【匹配模式】API 调用模式

### atomic_dirty_cacheline-RULE-02：UB 写越界 OOB 兜底（buf+OOB_OFFSET）

- 【条件】`__ubuf__ int32_t* oobPtr = buf + OOB_OFFSET` 使 UB 写地址远超声明大小
- 【API】`__ubuf__ int32_t* oobPtr = buf + OOB_OFFSET; *oobPtr = dirtyVal;`（OOB_OFFSET=65536）
- 【约束】buf 声明 TILE_LENGTH=256 int32_t = 1KB；OOB_OFFSET=65536 >> 256 → UB OOB → ADDR_MISALIGN
- 【匹配模式】值域判断

### atomic_dirty_cacheline-RULE-03：OOB_OFFSET 常量注入 + atomic 目标隔离

- 【条件】OOB_OFFSET 从 65536 改为 0，atomic 目标从 z[0] 改为 y[0]
- 【API】`constexpr uint32_t OOB_OFFSET = 65536;`（error）vs `constexpr uint32_t OOB_OFFSET = 0;`（fixed）
- 【约束】error 版 OOB_OFFSET=65536 + atomic 同址 z[0]；fixed 版 OOB_OFFSET=0 + atomic 目标 y[0]（结构隔离）
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：OOB_OFFSET 从 65536 改为 0（合法 UB 偏移）；atomic 目标改为 `&y[0]`（结构隔离，atomic 不命中 z[0] dirty cacheline）。

```cpp
// 修复前（error）
constexpr uint32_t OOB_OFFSET = 65536;
// ...
((__gm__ volatile int32_t*)z)[0] = dirtyVal;
asc_set_atomic_add_int();
asc_copy_ub2gm_sync((__gm__ void*)&z[0], (__ubuf__ void*)val, sizeof(int32_t));
asc_set_atomic_none();
__ubuf__ int32_t* oobPtr = buf + OOB_OFFSET;
*oobPtr = dirtyVal;

// 修复后（fixed）
constexpr uint32_t OOB_OFFSET = 0;
// ...
asc_set_atomic_add_int();
asc_copy_ub2gm_sync((__gm__ void*)&y[0], (__ubuf__ void*)val, sizeof(int32_t));
asc_set_atomic_none();
__ubuf__ int32_t* oobPtr = buf + OOB_OFFSET;
*oobPtr = dirtyVal;
```

**防御性编程建议**：

- atomic add 目标地址不应与先前的标量 store 地址重叠（结构隔离）
- UB 指针偏移前 assert `offset < TILE_LENGTH`
- atomic 操作前确保目标 cacheline 已 flush（无 dirty 状态）

## 变体索引

| 变体 | OOB_OFFSET | atomic 目标 | 触发路径 |
|------|-----------|------------|---------|
| 基准（本用例） | 65536 | z[0] | L102 + ADDR_MISALIGN |
| 仅主路径 | 0 | z[0] | L102 |
| 仅 OOB | 65536 | y[0] | ADDR_MISALIGN |
| 合法变体 | 0 | y[0] | 安全（fixed 版） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：atomic 目标隔离 + UB 合法 | OOB_OFFSET=0 且 atomic 目标 ≠ store 目标 | fixed 版（OOB=0, atomic→y[0]） |
| EC2：atomic 命中 dirty cacheline | 标量 store + atomic add 同址 | error 版主路径（z[0] store + atomic z[0]） |
| EC3：UB 写越界（OOB 兜底） | OOB_OFFSET > TILE_LENGTH | error 版兜底（65536 > 256） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| mte_atomic_misalign | 同族 atomic 类异常，地址不对齐路径 |
| scalar_int_neg_sqrt | OOB 兜底路径已验证用例，UB ADDR_MISALIGN |
| dvg_stack_overflow | 同族探索类，OOB 兜底结构 |

## 检测执行指引

- **适用匹配模式**：API 调用模式（RULE-01）、值域判断（RULE-02）、diff_pattern（RULE-03）。
- **命中判定**：按下列步骤核对 RULE-01 的 GM 标量 store 与 atomic add 同址顺序、RULE-02 的 UB 写越界和 RULE-03 的 error/fixed 差异；主路径与 OOB 兜底分别记录证据。
  1. 读取 error/main.asc 第 10 行 `OOB_OFFSET = 65536` 确认 OOB 兜底参数。
  2. 读取第 18 行 `((__gm__ volatile int32_t*)z)[0] = dirtyVal` 确认 GM 标量 store 创建 dirty cacheline。
  3. 读取第 20-23 行 `asc_set_atomic_add_int()` + `asc_copy_ub2gm_sync(&z[0], val, 4)` 确认 atomic add 同址命中 dirty cacheline。
  4. 读取第 26-27 行 `buf + OOB_OFFSET` + `*oobPtr = dirtyVal` 确认 OOB 兜底（65536 >> 256）。
  5. fixed 版第 10 行 `OOB_OFFSET = 0` + 第 23 行 `&y[0]`（结构隔离）→ 合法。
  6. error 版 stderr 应含 EZ9999（验证阶段补充 retCode/errcode/subErrType/vec error info）。
  - `runtime_required=true`：RULE-01 的 dirty cacheline 状态依赖运行时，需 retCode/errcode/subErrType/vec error info 确认 L102；仅有同址调用或 OOB 兜底的 EZ9999 不能确认主路径。
- **修复映射**：RULE-01 → 将 atomic 目标从 `&z[0]` 改为 `&y[0]`，与标量 store 目标隔离；RULE-02 → 将 `OOB_OFFSET` 从 65536 改为 0；RULE-03 → 同时采用目标隔离与合法 UB 偏移。
