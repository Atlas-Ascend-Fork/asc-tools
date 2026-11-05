# datacopy_burst_len_illegal — DataCopy 参数超限

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | datacopy_burst_len_illegal |
| 异常类型 | mte_gdma_illegal_burst_len（DataCopy 参数超限） |
| 错误码 | 30（mte_gdma_illegal_burst_len） |
| 类别 | datacopy |
| 源文件 | 来源: stable_ez9999 历史种子（legacy: N1） |
| 报错模式 | EZ9999_stderr |
| 内存区域 | GM（srcStride 使 GM 源端第 2 块读取地址越界） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

DataCopyParams 的参数被设置为非法值，导致 GDMA 硬件无法执行有效的 burst 搬运操作。原 历史种子（legacy: N1） 使用 `blockLen=0` 被 runtime 零值拦截；seed-N1s 使用 `srcStride=65535` 穿透 runtime 检查，使 GM 源端第 2 块的读取地址偏移 65535×32B ≈ 2MB 远超 GM 分配范围，直接触发硬件异常 `mte_gdma_illegal_burst_len`（错误码 30）。

- 违规模式（seed-N1s）：`DataCopyParams{2, 128, 65535, 0}` — srcStride=65535 使 GM 源端第 2 块读取地址偏移 65535×32B ≈ 2MB >> GM 分配范围，穿透 runtime 检查直接触发硬件异常（mte_gdma_illegal_burst_len，错误码 30）
- 与原 历史种子（legacy: N1）（blockLen=0 被零值拦截）的区别：srcStride=65535 穿透 runtime 检查直接触发硬件异常
- 安全模式：`DataCopyParams{1, 8, 0, 0}`（单块 8 个 32B 单元，stride=0，正常搬运）
- 最小触发条件：DataCopyParams 中 blockLen 为 0 或 srcStride 为超大值（≥65535），GDMA 硬件判定为非法 burst 参数

## 检测规则

### datacopy_burst_len_illegal-RULE-01：blockLen 直接赋值为 0

**【条件】**代码中出现 `params.blockLen = 0` 或 `DataCopyParams params{blockCount, 0, ...}`，blockLen 字段被显式赋值为 0
**【API】**DataCopyParams.blockLen 赋值
**【约束】**`blockLen > 0`
**【来源】**代码 diff 直接提取
**【匹配模式】** 值域判断

匹配示例（KB）：

- 原 历史种子（legacy: N1） 中 `DataCopyParams{1, 0, 0, 0}` — blockLen=0 被零值拦截

匹配示例（detector 补充）：

```cpp
DataCopyParams params;
params.blockLen = 0;
DataCopy(dst, src, params);
```

### datacopy_burst_len_illegal-RULE-02：tiling 参数驱动的 blockLen 为 0

**【条件】**`params.blockLen = tiling.FIELD` 或 `params.blockLen = A / B`（整数除法），当 tiling 字段或除法结果可能为 0 时触发
**【API】**DataCopyParams.blockLen 赋值（tiling 驱动）
**【约束】**`tiling.FIELD > 0` 且 `A >= B`（确保除法结果 ≥ 1）
**【来源】**LLM 推断
**【匹配模式】** 变量名匹配

匹配示例（detector 补充）：

```cpp
params.blockLen = tiling.tileLength / 32;
// 当 tileLength < 32 时结果为 0
```

### datacopy_burst_len_illegal-RULE-03：条件分支中 blockLen 未设置默认值

**【条件】**if-else/switch 分支中某路径遗漏 blockLen 赋值，走该分支时 blockLen 保持默认值 0
**【API】**DataCopyParams.blockLen 赋值（条件分支）
**【约束】**每条执行路径中 blockLen 均被赋有效正值
**【来源】**LLM 推断
**【匹配模式】** 存在性检查

匹配示例（detector 补充）：

```cpp
if (condition) {
    params.blockLen = value1;
} else {
    // blockLen 未赋值，保持默认 0
}
DataCopy(dst, src, params);
```

### datacopy_burst_len_illegal-RULE-STRIDE：DataCopy srcStride 超大值穿透

**【条件】**DataCopyParams 的 srcStride 字段为超大常量值（≥65535），且 GM buffer 分配大小远小于 stride × 32B，使第 2 块及后续块的 GM 源端读取地址越界
**【API】**DataCopy（DataCopyParams.srcStride）
**【约束】**`(blockCount * blockLen + (blockCount - 1) * srcStride) × 32B <= gmAllocSize`
**【来源】**运行验证
**【匹配模式】** 值域判断

匹配示例（KB）：

- seed-N1s 中 `DataCopyParams{2, 128, 65535, 0}` — srcStride=65535，第 2 块偏移 65535×32B ≈ 2MB >> GM 分配
- 覆盖种子：stable_ez9999 历史种子（legacy: N1s）

## 修复方案

### 标准修复模式

1. **修复模式 1（DataCopy 前检查 blockLen）**：`if (params.blockLen > 0) { DataCopy(...) }`
2. **修复模式 2（确保 blockLen 最小值）**：`params.blockLen = std::max(1u, computedBlockLen)`
3. **修复模式 3（修复 srcStride 超大值）**：将 srcStride 恢复为合法值（如 0），确保搬运地址在分配范围内
4. **修复模式 4（条件分支补全默认值）**：DataCopyParams 声明后立即设置合理默认值

### seed 具体修复

- seed-N1s：`DataCopyParams{2, 128, 65535, 0}` → `DataCopyParams{1, 8, 0, 0}`（单块 8 个 32B，stride=0）

### 代码模板

模板 1：安全的 DataCopy 封装（detector 补充）

```cpp
template<typename T>
__aicore__ inline void SafeDataCopy(LocalTensor<T>& dst, GlobalTensor<T>& src,
                                      DataCopyParams& params) {
    if (params.blockLen > 0 && params.blockCount > 0) {
        DataCopy(dst, src, params);
    }
}
```

模板 2：安全的 blockLen 计算（detector 补充）

```cpp
__aicore__ inline uint32_t ComputeSafeBlockLen(uint32_t totalSize, uint32_t unitSize) {
    if (totalSize < unitSize) {
        return 1;
    }
    return totalSize / unitSize;
}
```

### 防御性编程建议

1. **DataCopy 前检查**：每次调用 DataCopy 前验证 `blockLen > 0 && blockCount > 0`
2. **参数默认值**：DataCopyParams 声明后立即设置合理的默认 blockLen 值
3. **Host 侧 tiling 参数校验**：host 侧计算 tiling 时确保 derived blockLen 不为 0
4. **条件分支覆盖审查**：审查所有条件分支确保 blockLen 在每条路径上被正确赋值
5. **整数除法注意**：`a / b` 在 `a < b` 时结果为 0，需特殊处理
6. **封装安全 DataCopy helper**：将 DataCopy 调用封装到带参数校验的 helper 函数中

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| blockLen=0（零值拦截） | 原始 历史种子（legacy: N1）（TC-High） | `DataCopyParams{1, 0, 0, 0}`，blockLen=0 被 runtime 拦截 | EC2 |
| srcStride 超大值穿透 | stable_ez9999 历史种子（legacy: N1s） | `DataCopyParams{2, 128, 65535, 0}`，srcStride=65535 穿透 runtime | EC3 |
| tiling 驱动 blockLen 为 0 | 变体（待生成） | `params.blockLen = tiling.tileLength / 32` 当 tileLength < 32 | EC2 |
| 条件分支遗漏 blockLen 赋值 | 变体（待生成） | if-else 中某路径遗漏赋值 | EC2 |
| 动态计算 blockLen 为 0 | 变体（待生成） | `params.blockLen = remainingSize / 32` 当 remainingSize < 32 | EC2 |

## 等价类划分

变异维度：DataCopyParams 参数值（blockLen 和 srcStride）

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | blockLen > 0 且 srcStride 合法 | blockLen ∈ [1, bufferLen], srcStride × 32B × (blockCount-1) < allocSize | 参数合法，安全 |
| EC2 | blockLen = 0 | 0 | 零值参数被 runtime 拦截，覆盖原始 历史种子（legacy: N1） |
| EC3 | srcStride ≥ 65535 | ≥65535 | 超大值穿透 runtime，覆盖 seed-N1s |

划分依据：GDMA 硬件对 burst 参数的合法性检查。EC2 为软件层拦截，EC3 为硬件层触发。

## 关联异常

- **datacopy_burst_num_illegal**：共享 DataCopyParams 参数审查维度，datacopy_burst_len_illegal 关注 blockLen/srcStride，datacopy_burst_num_illegal 关注 blockCount/blockLen 超大值。
- **mte_write_ub_overflow/mte_read_ub_overflow**：共享 DataCopy 参数溢出机制，mte_write_ub_overflow/mte_read_ub_overflow 关注 stride 驱动的 UB 溢出，datacopy_burst_len_illegal 关注 stride 驱动的 GM 越界。
- **共享参数**：
  - （cross_references "参数共享"表收录）`blockLen → [datacopy_burst_len_illegal, datacopy_burst_num_illegal, datacopypad_ub_overflow]`、`dstStride → [datacopy_burst_len_illegal, datacopy_burst_num_illegal, datacopypad_ub_overflow]`。
  - （推断，非 cross_references 收录）与 mte_write_ub_overflow/mte_read_ub_overflow 共享 DataCopy stride 超限穿透模式（blockCount 条目仅收录 [datacopy_burst_num_illegal, mte_read_ub_overflow]，datacopy_burst_len_illegal 不在此条目中）。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/STRIDE）、变量名匹配（RULE-02）、存在性检查（RULE-03）
- **命中判定**：
  - 值域判断类规则——提取 `params.blockLen = VALUE` 表达式，VALUE 为编译期常量且 == 0 即命中；srcStride 场景验证 `(blockCount * blockLen + (blockCount - 1) * srcStride) × 32B <= gmAllocSize`；跨行追溯 ≤3 层
  - 变量名匹配类规则——在源码定位 `tiling.FIELD` 或 `params.blockLen = A / B` 表达式，追溯 tiling 字段赋值或除法操作数；追溯受限时标记 `trace_limited=true`
  - 存在性检查类规则——全文模式搜索 if-else/switch 分支结构，验证每条执行路径是否覆盖 blockLen 赋值（缺失即命中）
- **修复映射**：
  - datacopy_burst_len_illegal-RULE-01（blockLen 直接为 0）→ 修复模式 1（DataCopy 前检查）/ 修复模式 2（确保最小值）
  - datacopy_burst_len_illegal-RULE-02（tiling 驱动为 0）→ 修复模式 2
  - datacopy_burst_len_illegal-RULE-03（条件分支未设默认值）→ 修复模式 4（条件分支补全）
  - datacopy_burst_len_illegal-RULE-STRIDE（srcStride 超大值穿透）→ 修复模式 3（修复 srcStride）
