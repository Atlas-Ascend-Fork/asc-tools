# datacopy_burst_num_illegal — DataCopy 参数超限

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | datacopy_burst_num_illegal |
| 异常类型 | mte_gdma_illegal_burst_num（DataCopy 参数超限） |
| 错误码 | 31（mte_gdma_illegal_burst_num） |
| 类别 | datacopy |
| 源文件 | 来源: stable_ez9999 历史种子（legacy: N2） |
| 报错模式 | EZ9999_stderr |
| 内存区域 | UB（blockLen 使单块搬运超出 UB buffer 范围） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

DataCopyParams 的参数被设置为非法值，导致 GDMA 硬件无法执行有效的数据搬运操作。原 历史种子（legacy: N2） 使用 `blockCount=0` 被 runtime 零值拦截；seed-N2s 使用 `blockLen=65535` 穿透 runtime 检查，使单块搬运 65535×32B ≈ 2MB 远超 UB buffer 1024B，直接触发硬件异常 `mte_gdma_illegal_burst_num`（错误码 31）。

- 违规模式（seed-N2s）：`DataCopyParams{1, 65535, 0, 0}` — blockLen=65535 使单块搬运 65535×32B ≈ 2MB >> UB buffer 1024B，穿透 runtime 检查直接触发硬件异常（mte_gdma_illegal_burst_num，错误码 31）
- 与原 历史种子（legacy: N2）（blockCount=0 被零值拦截）的区别：blockLen=65535 穿透 runtime 检查
- 安全模式：`DataCopyParams{1, 8, 0, 0}`（正常搬运参数）
- 最小触发条件：DataCopyParams 中 blockCount 为 0 或 blockLen 为超大值（≥65535），GDMA 硬件判定为非法 burst 参数

## 检测规则

### datacopy_burst_num_illegal-RULE-01：blockCount 直接赋值为 0

**【条件】**代码中出现 `params.blockCount = 0` 或 `DataCopyParams params{0, blockLen, ...}`，blockCount 字段被显式赋值为 0
**【API】**DataCopyParams.blockCount 赋值
**【约束】**`blockCount > 0`
**【来源】**代码 diff 直接提取
**【匹配模式】** 值域判断

匹配示例（KB）：

- 原 历史种子（legacy: N2） 中 `DataCopyParams{0, 8, 0, 0}` — blockCount=0 被零值拦截

匹配示例（detector 补充）：

```cpp
DataCopyParams params;
params.blockCount = 0;
DataCopy(dst, src, params);
```

### datacopy_burst_num_illegal-RULE-02：tiling 参数驱动的 blockCount 为 0

**【条件】**`params.blockCount = tiling.FIELD` 或 `params.blockCount = A / B`（整数除法），当 tiling 字段或除法结果可能为 0 时触发
**【API】**DataCopyParams.blockCount 赋值（tiling 驱动）
**【约束】**`tiling.FIELD > 0` 且 `A >= B`（确保除法结果 ≥ 1）
**【来源】**LLM 推断
**【匹配模式】** 变量名匹配

匹配示例（detector 补充）：

```cpp
params.blockCount = tiling.totalElements / tiling.elementsPerBlock;
// 当 totalElements < elementsPerBlock 时结果为 0
```

### datacopy_burst_num_illegal-RULE-03：多维搬运维度为 0

**【条件】**`params.blockCount = dimH * dimW` 或 `params.blockCount = axisSize / tileSize`，当某维度或除法结果为 0 时触发
**【API】**DataCopyParams.blockCount 赋值（多维计算）
**【约束】**每个维度 > 0 且 `axisSize >= tileSize`
**【来源】**LLM 推断
**【匹配模式】** 值域判断

匹配示例（detector 补充）：

```cpp
params.blockCount = dimH * dimW;
params.blockCount = axisSize / tileSize;
```

### datacopy_burst_num_illegal-RULE-BLOCKLEN：DataCopy blockLen 超大值穿透

**【条件】**DataCopyParams 的 blockLen 字段为超大常量值（≥65535），且 UB buffer 分配大小远小于 blockLen × 32B，使单块搬运超出 UB buffer 范围
**【API】**DataCopy（DataCopyParams.blockLen）
**【约束】**`blockLen × 32B <= ubAllocSize`
**【来源】**运行验证
**【匹配模式】** 值域判断

匹配示例（KB）：

- seed-N2s 中 `DataCopyParams{1, 65535, 0, 0}` — blockLen=65535，单块搬运 65535×32B ≈ 2MB >> UB buffer 1024B
- 覆盖种子：stable_ez9999 历史种子（legacy: N2s）

## 修复方案

### 标准修复模式

1. **修复模式 1（DataCopy 前检查 blockCount）**：`if (params.blockCount > 0) { DataCopy(...) }`
2. **修复模式 2（确保 blockCount 最小值）**：`params.blockCount = std::max(1u, computedBlockCount)`
3. **修复模式 3（修复 blockLen 超大值）**：将 blockLen 恢复为合法值，确保 `blockLen × 32B <= ubAllocSize`
4. **修复模式 4（多维搬运维度校验）**：多维搬运前检查每个维度是否大于 0

### seed 具体修复

- seed-N2s：`DataCopyParams{1, 65535, 0, 0}` → `DataCopyParams{1, 8, 0, 0}`（单块 8 个 32B，正常搬运）

### 代码模板

模板 1：安全的 blockCount 计算（detector 补充）

```cpp
__aicore__ inline uint32_t ComputeSafeBlockCount(uint32_t totalElements,
                                                   uint32_t elementsPerBlock) {
    if (totalElements == 0 || elementsPerBlock == 0) {
        return 0;
    }
    return (totalElements + elementsPerBlock - 1) / elementsPerBlock;
}
```

模板 2：安全的 DataCopy 执行（detector 补充）

```cpp
template<typename T>
__aicore__ inline void SafeDataCopyWithCheck(LocalTensor<T>& dst,
                                               GlobalTensor<T>& src,
                                               uint32_t blockCount,
                                               uint32_t blockLen) {
    if (blockCount > 0 && blockLen > 0) {
        DataCopyParams params;
        params.blockCount = blockCount;
        params.blockLen = blockLen;
        params.srcStride = 1;
        params.dstStride = 1;
        DataCopy(dst, src, params);
    }
}
```

### 防御性编程建议

1. **DataCopy 前检查**：每次调用 DataCopy 前验证 `blockCount > 0 && blockLen > 0`
2. **向上取整除法**：使用 `(a + b - 1) / b` 避免 `a < b` 时结果为 0
3. **Host 侧 tiling 参数校验**：host 侧计算 tiling 时确保 derived blockCount 不为 0
4. **多维维度检查**：多维搬运前检查每个维度是否大于 0
5. **零数据量处理**：当数据总量为 0 时直接返回，不执行 DataCopy
6. **封装安全 helper**：将 blockCount 计算和 DataCopy 调用封装到带校验的 helper 中

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| blockCount=0（零值拦截） | 原始 历史种子（legacy: N2）（TC-High） | `DataCopyParams{0, 8, 0, 0}`，blockCount=0 被 runtime 拦截 | EC2 |
| blockLen 超大值穿透 | stable_ez9999 历史种子（legacy: N2s） | `DataCopyParams{1, 65535, 0, 0}`，blockLen=65535 穿透 runtime | EC3 |
| tiling 驱动 blockCount 为 0 | 变体（待生成） | `params.blockCount = tiling.totalBlocks` 当 totalBlocks=0 | EC2 |
| 多维搬运维度为 0 | 变体（待生成） | `params.blockCount = dimH * dimW` 当 dimH 或 dimW 为 0 | EC2 |

## 等价类划分

变异维度：DataCopyParams 参数值（blockCount 和 blockLen）

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | blockCount > 0 且 blockLen 合法 | blockCount ∈ [1, ...], blockLen × 32B <= ubAllocSize | 参数合法，安全 |
| EC2 | blockCount = 0 | 0 | 零值参数被 runtime 拦截，覆盖原始 历史种子（legacy: N2） |
| EC3 | blockLen ≥ 65535 | ≥65535 | 超大值穿透 runtime，覆盖 seed-N2s |

划分依据：GDMA 硬件对 burst 参数的合法性检查。EC2 为软件层拦截，EC3 为硬件层触发。

## 关联异常

- **datacopy_burst_len_illegal**：共享 DataCopyParams 参数审查维度，datacopy_burst_num_illegal 关注 blockCount/blockLen，datacopy_burst_len_illegal 关注 blockLen/srcStride 超限。
- **mte_write_ub_overflow/mte_read_ub_overflow**：共享 DataCopy 参数溢出机制，mte_write_ub_overflow/mte_read_ub_overflow 关注 stride 驱动的 UB 溢出。
- **共享参数**：（cross_references "参数共享"表收录）`blockLen → [datacopy_burst_len_illegal, datacopy_burst_num_illegal, datacopypad_ub_overflow]`、`blockCount → [datacopy_burst_num_illegal, mte_read_ub_overflow]`、`dstStride → [datacopy_burst_len_illegal, datacopy_burst_num_illegal, datacopypad_ub_overflow]`。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/03/BLOCKLEN）、变量名匹配（RULE-02）
- **命中判定**：
  - 值域判断类规则——提取 `params.blockCount = VALUE` 表达式，VALUE 为编译期常量且 == 0 即命中；blockLen 场景验证 `blockLen × 32B <= ubAllocSize`；多维场景验证每个维度 > 0；跨行追溯 ≤3 层
  - 变量名匹配类规则——在源码定位 `tiling.FIELD` 或 `params.blockCount = A / B` 表达式，追溯 tiling 字段或除法操作数；追溯受限时标记 `trace_limited=true`
- **修复映射**：
  - datacopy_burst_num_illegal-RULE-01（blockCount 直接为 0）→ 修复模式 1（DataCopy 前检查）/ 修复模式 2（确保最小值）
  - datacopy_burst_num_illegal-RULE-02（tiling 驱动为 0）→ 修复模式 2
  - datacopy_burst_num_illegal-RULE-03（多维维度为 0）→ 修复模式 4（多维维度校验）
  - datacopy_burst_num_illegal-RULE-BLOCKLEN（blockLen 超大值穿透）→ 修复模式 3（修复 blockLen）
