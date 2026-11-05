# datacopypad_ub_overflow — DataCopyPad 写 UB 溢出

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | datacopypad_ub_overflow |
| 异常类型 | mte_ub_wr_ovflw |
| 错误码 | 124（与 mte_write_ub_overflow 共享，碰撞） |
| 类别 | datacopy |
| 源文件 | cann-aicore/05_datacopy/datacopypad_oob_verified/error/main.asc |
| 报错模式 | EZ9999_stderr |
| 内存区域 | UB（DataCopyPad 写入 UB 侧溢出 buffer 边界） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

DataCopyPad 操作的 `blockLen + paddingSize` 超出目标 UB（Unified Buffer）buffer 的实际容量时，写入操作溢出 buffer 边界。GDMA 在执行 DataCopyPad 时会在 blockLen 数据之后写入 padding 填充值，当总写入量超过 buffer 分配大小时触发 `mte_ub_wr_ovflw` 异常（错误码 124，与 mte_write_ub_overflow 碰撞）。此问题常见于 buffer 分配时未考虑 padding 开销或 padding 参数设置不当。同属 cross_references "症状相似"段：UB overflow = {vec_ub_overflow, mte_write_ub_overflow, mte_read_ub_overflow, datacopypad_ub_overflow}（区分要点：datacopypad_ub_overflow = DataCopyPad 参数导致 UB 写越界）。

触发条件（detector 原文，逐字保留）：

- DataCopyPad 的 `paddingSize` 设置过大，导致 blockLen + padding 超出 buffer
- UB buffer 声明大小不足以容纳 blockLen 数据 + padding 填充
- leftPadding + rightPadding 总和过大
- tiling 驱动的 padding 参数未经边界校验

形式化规则前提：`ASSERT datacopypad_ub_overflow-OVERFLOW-01: ∀ DataCopyPad 调用 C 使用 buffer B: blockLen + paddingSize ≤ sizeof(B) / sizeof(element_type)`。

## 检测规则

### datacopypad_ub_overflow-RULE-01：paddingSize 与 buffer 大小不匹配

**【条件】**`__ubuf__ TYPE dstLocal[N]` 声明的 buffer，DataCopyPadParams 中 `blockLen + paddingSize > N`（或 `blockLen × sizeof(TYPE) + paddingSize × sizeof(TYPE) > N × sizeof(TYPE)`）
**【API】**DataCopyPadParams.blockLen / paddingSize × `__ubuf__` buffer 声明大小
**【约束】**`blockLen + paddingSize ≤ sizeof(B) / sizeof(element_type)`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
// 匹配：buffer 容量不足
__ubuf__ half dstLocal[256];
params.blockLen = 256;
params.paddingSize = 128;
// 256 + 128 = 384 > 256，溢出
```

### datacopypad_ub_overflow-RULE-02：leftPadding + rightPadding 总和过大

**【条件】**DataCopyPadParams 中 `leftPadding + rightPadding` 总和过大，`blockLen + leftPadding + rightPadding` 超过 buffer 容量
**【API】**DataCopyPadParams.leftPadding / rightPadding / blockLen
**【约束】**`blockLen + leftPadding + rightPadding ≤ bufferCapacity`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
// 匹配：双向 padding 总和过大
params.leftPadding = 64;
params.rightPadding = 64;
params.blockLen = 256;
// 256 + 64 + 64 = 384，超出 buffer
```

### datacopypad_ub_overflow-RULE-03：tiling 驱动的 padding 参数

**【条件】**`paddingSize = tiling.padSize` 或 `paddingSize = computePadding(tiling)`，padding 参数来源于 tiling 且缺少上限约束
**【API】**DataCopyPadParams.paddingSize（tiling 驱动）
**【约束】**`tiling.paddingSize < tiling.bufferCapacity ∧ tiling.blockLen + tiling.paddingSize ≤ tiling.bufferCapacity`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 变量名匹配

匹配示例（detector 原文）：

```cpp
params.paddingSize = tiling.padSize;
params.paddingSize = computePadding(tiling);
```

### datacopypad_ub_overflow-RULE-04：buffer 容量与写入量关系检查（运行时检测）

**【条件】**DataCopyPad 调用前未检查 `blockLen + paddingSize ≤ bufferCapacity`
**【API】**DataCopyPad（调用前容量校验）
**【约束】**DataCopyPad 调用前存在 `ASSERT(blockLen + paddingSize ≤ bufferCapacity)` 校验
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

### datacopypad_ub_overflow-RULE-05：padding 参数范围校验（运行时检测）

**【条件】**padding 参数未校验是否 `paddingSize ≤ MAX_PADDING` 且 `paddingSize < bufferCapacity`
**【API】**DataCopyPadParams.paddingSize（运行时范围校验）
**【约束】**`paddingSize ≤ MAX_PADDING ∧ paddingSize < bufferCapacity`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

## 修复方案

### 标准修复模式

1. **修复模式 1（扩大 buffer 容量）**：`__ubuf__ half dstLocal[512];`（声明时预留 padding 空间）
2. **修复模式 2（减小 padding 参数）**：`params.paddingSize = 16;`（合理上限）
3. **修复模式 3（动态校验写入量）**：`uint32_t totalWrite = params.blockLen + params.paddingSize; if (totalWrite <= BUFFER_CAPACITY) { DataCopyPad(...); }`
4. **修复模式 4（修复 leftPadding + rightPadding）**：`params.leftPadding = 16; params.rightPadding = 16;`（合理双向 padding）

### 代码模板

模板：安全的 DataCopyPad 封装（detector 原文）

```cpp
template<typename T, padType pad>
__aicore__ inline void SafeDataCopyPad(LocalTensor<T>& dst,
                                        GlobalTensor<T>& src,
                                        uint32_t blockLen,
                                        uint32_t paddingSize,
                                        uint32_t bufferCapacity) {
    if (blockLen + paddingSize <= bufferCapacity) {
        DataCopyPadParams<pad> params;
        params.blockLen = blockLen;
        params.paddingSize = paddingSize;
        DataCopyPad(dst, src, params);
    }
}
```

模板：带 padding 预留的 buffer 分配（detector 原文）

```cpp
template<typename T, uint32_t DATA_SIZE, uint32_t MAX_PADDING>
struct PaddedBuffer {
    static constexpr uint32_t TOTAL_SIZE = DATA_SIZE + MAX_PADDING;
    __ubuf__ T buffer[TOTAL_SIZE];
    static_assert(TOTAL_SIZE <= MAX_UB_ELEMENTS, "buffer exceeds UB capacity");
};
```

### 防御性编程建议

1. **预留 padding 空间**：声明 UB buffer 时额外分配 padding 所需空间
2. **padding 上限约束**：在 tiling 计算中限制 paddingSize 的最大值
3. **写入量校验**：DataCopyPad 调用前检查 `blockLen + paddingSize <= bufferCapacity`
4. **统一 buffer 模板**：使用模板参数管理 buffer 大小和 padding 上限
5. **Tiling 参数联动**：padding 参数与 buffer 分配大小联动计算
6. **静态断言**：对编译期可确定的 buffer 和 padding 使用 `static_assert` 检查

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| paddingSize 过大 | cann-aicore/05_datacopy/datacopypad_oob_verified | `params.paddingSize = 128` 配合较小 buffer 导致溢出 | EC2 |
| blockLen + paddingSize > bufferSize | 变体（待生成） | buffer 大小未预留 padding 空间 | EC2 |
| leftPadding + rightPadding 总和过大 | 变体（待生成） | 双向 padding 累计超出 buffer 容量 | EC2 |
| tiling 驱动的 padding 参数 | 变体（待生成） | `params.paddingSize = tiling.padSize` tiling 计算错误导致 padding 过大 | EC3 |

## 等价类划分

变异维度：DataCopyPadParams.blockLen + paddingSize（或 leftPadding + rightPadding）相对于 UB buffer 容量

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | blockLen + paddingSize ≤ bufferCapacity | 合法范围 | 写入范围在 buffer 内，安全 |
| EC2 | blockLen + paddingSize > bufferCapacity | 编译期可确定 | 覆盖原始种子 |
| EC3 | tiling 驱动的 padding 参数越界 | tiling 计算错误 | 需运行时检查 |

划分依据：GDMA 对 DataCopyPad 写入 UB 的地址范围检查。EC2 为编译期可确定的 padding/buffer 不匹配，EC3 需 host 侧 tiling 计算路径分析（运行时检查）。

## 关联异常

- **vec_ub_overflow / mte_write_ub_overflow / mte_read_ub_overflow**：同属 cross_references "症状相似"段：UB overflow = {vec_ub_overflow, mte_write_ub_overflow, mte_read_ub_overflow, datacopypad_ub_overflow}。区分要点（cross_references 原文）：vec_ub_overflow = Vector 操作跨度超出 UB 边界；mte_write_ub_overflow = MTE 写 UB 地址溢出；mte_read_ub_overflow = MTE 读 UB 地址溢出；datacopypad_ub_overflow = DataCopyPad 参数导致 UB 写越界。
- **mte_write_ub_overflow**：错误码 124 双重关联——同症状组 + 同错误码碰撞（error_code_index.md §碰撞标注：124 → [datacopypad_ub_overflow, mte_write_ub_overflow]，均为 mte_ub_wr_ovflw）。区分：mte_write_ub_overflow 为 DataCopy 的 dstStride 参数导致，datacopypad_ub_overflow 为 DataCopyPad 的 padding 参数导致。
- **blockLen / dstStride / padding**：共享参数（cross_references "参数共享"表收录：`blockLen → [datacopy_burst_len_illegal, datacopy_burst_num_illegal, datacopypad_ub_overflow]`、`dstStride → [datacopy_burst_len_illegal, datacopy_burst_num_illegal, datacopypad_ub_overflow]`、`padding → [padding_misconfig]`）。datacopypad_ub_overflow 在 blockLen 与 dstStride 两个条目中。
- **DataCopyPad**：共享 API（cross_references 反查 api_index：DataCopyPad → [datacopypad_ub_overflow]）。
- **（推断，非 cross_references 收录）**与 mte_write_ub_overflow 共享 mte_ub_wr_ovflw 异常类型与 UB 写溢出机制，但 API 不同（datacopypad_ub_overflow = DataCopyPad padding 驱动，mte_write_ub_overflow = DataCopy dstStride 驱动）。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02/05）、变量名匹配（RULE-03）、存在性检查（RULE-04）
- **命中判定**：
  - 值域判断类规则——提取 `__ubuf__ TYPE dstLocal[N]` 与 `params.blockLen = L; params.paddingSize = P;`，计算 `L + P` 与 N 比对（或 `L + leftPadding + rightPadding` 与 N 比对），超限即命中 RULE-01/02；padding 范围校验 `paddingSize ≤ MAX_PADDING ∧ paddingSize < bufferCapacity` 失败即命中 RULE-05；跨行追溯 ≤3 层
  - 变量名匹配类规则——在源码定位 `params.paddingSize = tiling.padSize` 或 `params.paddingSize = computePadding(tiling)` 表达式，追溯 host 侧 tiling 计算；追溯受限时标记 `trace_limited=true`
  - 存在性检查类规则——DataCopyPad 调用前是否存在 `blockLen + paddingSize ≤ bufferCapacity` 断言（缺失即命中 RULE-04）
- **修复映射**：
  - datacopypad_ub_overflow-RULE-01（paddingSize 与 buffer 不匹配）→ 修复模式 1（扩大 buffer）/ 修复模式 2（减小 padding）
  - datacopypad_ub_overflow-RULE-02（leftPadding + rightPadding 过大）→ 修复模式 4（修复双向 padding）
  - datacopypad_ub_overflow-RULE-03（tiling 驱动 padding）→ 修复模式 3（动态校验写入量）/ 防御建议 2（padding 上限约束）
  - datacopypad_ub_overflow-RULE-04（容量校验缺失）→ 修复模式 3（添加 ASSERT）
  - datacopypad_ub_overflow-RULE-05（padding 范围校验）→ 修复模式 2 / 修复模式 3
