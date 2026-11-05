# vec_ub_overflow — Vector UB 越界

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | vec_ub_overflow |
| 异常类型 | vec_ub_addr_wrap_around |
| 错误码 | 154 |
| 类别 | memory_overflow |
| 源文件 | cann-aicore/14_ub_overflow/vector_ub_oob_verified/error/main.asc |
| 报错模式 | EZ9999_stderr |
| 内存区域 | UB（Vector 操作地址回绕超出 192KB UB 容量） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

> **容量适用范围**：本文容量数值记录历史种子环境，不代表所有芯片或 tensor 的可访问容量。当前输入须先确认架构、API 重载及实际分配边界；未知时标记 `runtime_required=true`。统一单位与计分口径见 `references/constraints/semantic_contract.md`。

当 Vector 计算单元访问 UB（Unified Buffer）时，若操作的地址超出 192KB（196608 字节）容量限制，地址将发生回绕（wrap around），触发 vec_ub_overflow 异常（vec_ub_addr_wrap_around，错误码 154）。注意区分：ub_capacity_overflow 是总分配超过 UB 容量限制，vec_ub_overflow 是单次操作的地址发生回绕。同属 cross_references "症状相似"段：UB overflow = {vec_ub_overflow, mte_write_ub_overflow, mte_read_ub_overflow, datacopypad_ub_overflow}（区分要点：vec_ub_overflow = Vector 操作跨度超出 UB 边界）。

触发条件（detector 原文，逐字保留）：

1. **UB buffer 声明超过 192KB**：`__ubuf__` 声明的 buffer 大小超过 192KB
2. **Vector 操作地址回绕**：Vector 操作的地址偏移超出 192KB 导致地址回绕
3. **累积偏移超出 UB**：循环中地址累积偏移超出 192KB 上限
4. **与 ub_capacity_overflow 混淆**：ub_capacity_overflow 是总分配超限，vec_ub_overflow 是地址回绕

形式化规则前提：`ASSERT_vec_ub_overflow_1: ∀ __ubuf__ buffer, buffer_size ≤ UB_CAPACITY (192KB = 196608 bytes)`；`ASSERT_vec_ub_overflow_2/3: ∀ Vector 操作, dst_addr + operation_length ≤ UB_CAPACITY ∧ src_addr + operation_length ≤ UB_CAPACITY`。

## 检测规则

### vec_ub_overflow-RULE-01：__ubuf__ buffer 大小检测

**【条件】**`__ubuf__` 声明的 buffer，`buffer_element_count × sizeof(element) > 192KB (196608 bytes)`
**【API】**`__ubuf__` buffer 声明（数组大小 × sizeof(element_type)）
**【约束】**`buffer_element_count × sizeof(element) ≤ UB_CAPACITY (196608)`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
// 匹配：UB buffer 超过 192KB
__ubuf__ float largeBuf[65536];     // 256KB
__ubuf__ half hugeBuf[200000];      // 400KB
__ubuf__ int32_t bigBuf[60000];     // 240KB
```

不匹配（detector 原文）：

```cpp
// 不匹配：UB buffer 在 192KB 以内
__ubuf__ float smallBuf[49152];     // 192KB
__ubuf__ half normalBuf[98304];     // 192KB
__ubuf__ int32_t buf[40000];        // 160KB
```

### vec_ub_overflow-RULE-02：Vector 操作地址范围检测

**【条件】**Vector 计算（Add、Mul、Exp 等）操作 __ubuf__ buffer，操作的源地址或目标地址偏移 + 操作长度超出 192KB
**【API】**Add / Mul / Exp / Adds（Vector 计算，__ubuf__ buffer 偏移 + 长度）
**【约束】**`dst_addr + operation_length ≤ UB_CAPACITY ∧ src_addr + operation_length ≤ UB_CAPACITY`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
// 匹配：Vector 操作地址超限
Add(dstBuf + 50000, srcBuf + 50000, srcBuf2, 10000);
// 50000 * 4 + 10000 * 4 = 240KB > 192KB
```

不匹配（detector 原文）：

```cpp
// 不匹配：Vector 操作地址合法
Add(dstBuf + 1000, srcBuf + 1000, srcBuf2, 10000);
// 1000 * 4 + 10000 * 4 = 44KB < 192KB
```

### vec_ub_overflow-RULE-03：与 ub_capacity_overflow 异常区分检测

**【条件】**需区分总 UB 分配超限（ub_capacity_overflow）与单次地址回绕（vec_ub_overflow）：ub_capacity_overflow 场景为多个 `__ubuf__` buffer 总分配超出 192KB；vec_ub_overflow 场景为单个操作的地址偏移超出 192KB 导致回绕
**【API】**`__ubuf__` buffer 总分配（ub_capacity_overflow）vs 单次 Vector 操作偏移（vec_ub_overflow）
**【约束】**ub_capacity_overflow: `Σ buffer_size ≤ UB_CAPACITY`；vec_ub_overflow: `单次操作地址 + 长度 ≤ UB_CAPACITY`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

匹配示例（detector 原文）：

```cpp
// vec_ub_overflow 场景：单次操作地址回绕
__ubuf__ float buf[49152];
// 合法分配，但操作偏移超出
Adds(buf + 52000, src, len);  // 地址回绕

// ub_capacity_overflow 场景：总分配超限
__ubuf__ float buf1[32768];  // 128KB
__ubuf__ float buf2[32768];  // 128KB
// 总共 256KB > 192KB
```

### vec_ub_overflow-RULE-04：累积偏移运行时检测

**【条件】**循环中累积偏移需验证不超出 192KB UB 上限
**【API】**循环内 Vector 操作（累积偏移）
**【约束】**`ASSERT(ubOffset + operationLength <= UB_CAPACITY) ∧ ASSERT(ubOffset >= 0)`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

## 修复方案

### 标准修复模式

1. **修复模式 1（缩小 UB buffer 声明）**：`__ubuf__ float smallBuf[49152];`（192KB = 196608 bytes = 49152 floats）
2. **修复模式 2（tiling 分块控制 UB 用量）**：通过 tiling 策略控制单次 Vector 操作的数据量在 UB 容量内
3. **修复模式 3（Vector 操作地址范围校验）**：`if (offset + len <= UB_CAPACITY_FLOATS) { Add(buf + offset, src1 + offset, src2, len); }`
4. **修复模式 4（区分 ub_capacity_overflow 和 vec_ub_overflow 的修复策略）**：ub_capacity_overflow 修复总分配量（如 `buf1[24576] + buf2[24576] = 192KB`），vec_ub_overflow 修复单次操作的地址范围（`ASSERT(addr + len × sizeof(float) <= UB_BYTES)`）

### 代码模板

模板 1：安全的 UB 分配与操作工具（detector 原文）

```cpp
constexpr uint64_t UB_CAPACITY = 196608;

__aicore__ inline void SafeUbVectorOp(
    LocalTensor<float>& dst, LocalTensor<float>& src1, LocalTensor<float>& src2,
    uint64_t dstOffset, uint64_t srcOffset, uint64_t len) {
    ASSERT(dstOffset * sizeof(float) + len * sizeof(float) <= UB_CAPACITY);
    ASSERT(srcOffset * sizeof(float) + len * sizeof(float) <= UB_CAPACITY);
    Add(dst[dstOffset], src1[srcOffset], src2[srcOffset], len);
}
```

### 防御性编程建议

1. **UB buffer 声明总量不超过 192KB**：所有 `__ubuf__` buffer 的总大小严格控制在 196608 字节以内
2. **使用 tiling 控制单次操作数据量**：通过 tiling 策略确保每次 Vector 操作的数据在 UB 容量内
3. **Vector 操作前检查地址范围**：确保 `base_offset + operation_length <= UB_CAPACITY`
4. **区分 ub_capacity_overflow 和 vec_ub_overflow 的修复方向**：ub_capacity_overflow 修复总分配量，vec_ub_overflow 修复单次操作的地址范围
5. **使用常量定义 UB 容量**：在代码中使用 `constexpr uint64_t UB_CAPACITY = 196608` 替代硬编码数值
6. **循环累积偏移添加上界检查**：在循环递增偏移时添加 `offset < UB_CAPACITY` 的终止条件

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| UB buffer 声明超过 192KB | cann-aicore/14_ub_overflow/vector_ub_oob_verified | `__ubuf__ float buf[65536]`（256KB） | EC2 |
| Vector 操作地址回绕 | 变体（待生成） | `buf[50000]` 超出 192KB（Vector 操作偏移超限） | EC2 |
| 累积偏移超出 UB | 变体（待生成） | `buf[i * stride]` 循环累积超限 | EC3 |
| 与 ub_capacity_overflow 区分 | 变体（待生成） | 诊断区分 ub_capacity_overflow（总分配超限）vs vec_ub_overflow（地址回绕） | - |

## 等价类划分

变异维度：__ubuf__ buffer 大小 / Vector 操作地址偏移相对于 192KB UB 容量

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | buffer_size ≤ UB ∧ 操作地址 + 长度 ≤ UB | 合法范围 | 安全 |
| EC2 | 单次声明或操作地址超 192KB | buffer > 192KB 或 offset + len > 192KB | 编译期可确定，覆盖原始种子 |
| EC3 | 循环累积偏移超 192KB | 累积 offset > 192KB | 需运行时分析 |

划分依据：Vector 计算单元对 UB 地址的范围检查。EC2 为编译期可确定的单次超限，EC3 需循环累积偏移运行时分析。

## 关联异常

- **mte_write_ub_overflow / mte_read_ub_overflow / datacopypad_ub_overflow**：同属 cross_references "症状相似"段：UB overflow = {vec_ub_overflow, mte_write_ub_overflow, mte_read_ub_overflow, datacopypad_ub_overflow}。区分要点（cross_references 原文）：vec_ub_overflow = Vector 操作跨度超出 UB 边界；mte_write_ub_overflow = MTE 写 UB 地址溢出；mte_read_ub_overflow = MTE 读 UB 地址溢出；datacopypad_ub_overflow = DataCopyPad 参数导致 UB 写越界。
- **ub_capacity_overflow**：共享 UB 容量约束（区分检测见 RULE-03）。ub_capacity_overflow 不在 cross_references "症状相似" UB overflow 组中（组内仅 {vec_ub_overflow, mte_write_ub_overflow, mte_read_ub_overflow, datacopypad_ub_overflow}），属推断关联。
- **Add / Adds**：共享 API（cross_references 反查 api_index：Add → [ccu_div0, vec_ub_overflow]，Adds → [vec_ub_overflow]）。
- **（推断，非 cross_references 收录）**与 mte_write_ub_overflow/mte_read_ub_overflow 共享 UB 地址溢出机制（vec_ub_overflow = Vector ops 触发，mte_write_ub_overflow = MTE 写 UB 触发，mte_read_ub_overflow = MTE 读 UB 触发）；与 ub_capacity_overflow 共享 UB_CAPACITY 约束（ub_capacity_overflow = 总分配超限，vec_ub_overflow = 单次地址回绕）。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02）、API 调用模式（RULE-03）、存在性检查（RULE-04）
- **命中判定**：
  - 值域判断类规则——提取 `__ubuf__ TYPE buf[N]` 声明，计算 `N × sizeof(TYPE)` 与 UB_CAPACITY (196608) 比对，超限即命中 RULE-01；提取 Vector 操作 `Op(buf + OFFSET, ..., LEN)`，计算 `OFFSET × sizeof(TYPE) + LEN × sizeof(TYPE)` 与 UB_CAPACITY 比对，超限即命中 RULE-02；跨行追溯 ≤3 层
  - API 调用模式类规则——统计 `__ubuf__` buffer 总分配量（用于区分 ub_capacity_overflow）vs 单次 Vector 操作偏移（用于确认 vec_ub_overflow）；正确路由异常编号
  - 存在性检查类规则——循环内累积偏移变量（如 `offset += stride`）是否存在 `offset < UB_CAPACITY` 上界守卫或 `ASSERT(offset + len <= UB_CAPACITY)` 断言（缺失即命中 RULE-04）
- **修复映射**：
  - vec_ub_overflow-RULE-01（buffer 声明超 192KB）→ 修复模式 1（缩小 buffer）/ 修复模式 2（tiling 分块）
  - vec_ub_overflow-RULE-02（Vector 操作地址超限）→ 修复模式 3（地址范围校验）
  - vec_ub_overflow-RULE-03（与 ub_capacity_overflow 区分）→ 用于诊断路由，ub_capacity_overflow 用修复模式 4 的 ub_capacity_overflow 分支，vec_ub_overflow 用 vec_ub_overflow 分支
  - vec_ub_overflow-RULE-04（累积偏移）→ 修复模式 2（tiling 分块）/ 防御建议 6（循环上界守卫）
