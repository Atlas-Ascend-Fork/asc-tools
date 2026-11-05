# loaddata3d_read_overflow — LoadData 3D 读越界

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | loaddata3d_read_overflow |
| 异常类型 | mte_err_read_3d_overflow |
| 错误码 | 93 |
| 类别 | loaddata |
| 源文件 | cann-aicore/08_loaddata/ld3d_read_oob_verified/error/main.asc |
| 报错模式 | EZ9999_stderr |
| 内存区域 | L0_A（A1 buffer 读越界；A1 属 L0A 侧） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

LoadData3D 指令执行时，源 A1 buffer 的分配大小不足以提供读取所需的数据量。当通过 `qA1.InitBuffer` 分配的 A1 buffer 过小（如仅分配 64 字节），而 LoadData3D 的读取参数要求的读取量超过 buffer 容量时，MTE 检测到读操作越界并触发 mte_err_read_3d_overflow 异常（错误码 93）。

触发条件（detector 原文，逐字保留）：

- A1 buffer 的 InitBuffer 分配大小过小（如仅 64 字节）
- LoadData3D 的读取量超出 A1 buffer 实际容量
- srcStride 参数过大导致读取地址超出 A1 范围
- tiling 驱动的读取参数计算结果越界

形式化规则前提：`ASSERT loaddata3d_read_overflow-BUF-01: ∀ LoadData3D 调用: max_read_addr = (PARAMS.repeat - 1) × PARAMS.srcStride + PARAMS.channelSize × sizeof(element) ≤ a1_size`；`ASSERT loaddata3d_read_overflow-BUF-02: srcStride × (repeat - 1) + channelSize × sizeof(element) ≤ A1_buffer_size`。

## 检测规则

### loaddata3d_read_overflow-RULE-01：A1 buffer 分配过小

**【条件】**`qA1.InitBuffer(TYPE*, SIZE)` 分配大小 SIZE，`LoadData3D(DST, A1_BUF, PARAMS)` 读取量超过 SIZE
**【API】**qA1.InitBuffer 分配大小 × LoadData3D 读取量
**【约束】**`SIZE ≥ LoadData3D 最大读取量`（基于 srcStride、channelSize、repeat 等参数）
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
qA1.InitBuffer(half*, 64);
LoadData3D(a2, a1d, ldParams);
```

### loaddata3d_read_overflow-RULE-02：LoadData3D 读取参数超出 A1 容量

**【条件】**`ldParams.srcStride = STRIDE; ldParams.channelSize = CHANNEL; ldParams.repeat = REPEAT;`，最大读取地址 `(repeat - 1) × srcStride + channelSize × sizeof(element)` 超出 A1 buffer 的 InitBuffer 大小
**【API】**LoadData3DParams.srcStride × (repeat - 1) + channelSize × sizeof(element)
**【约束】**`max_read_addr ≤ A1_buffer_size`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

### loaddata3d_read_overflow-RULE-03：srcStride 导致读取越界

**【条件】**`srcStride × (repeat - 1) + channelSize` 超出 A1 范围，srcStride 步进后的地址不再在 A1 buffer 内
**【API】**LoadData3DParams.srcStride
**【约束】**`srcStride × (repeat - 1) + channelSize × sizeof(element) ≤ A1_buffer_size`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
ldParams.srcStride = STRIDE;
```

### loaddata3d_read_overflow-RULE-04：Tiling 驱动的读取参数越界（运行时检测）

**【条件】**tiling 参数中影响 LoadData3D 读取量的字段（srcStride、channelSize）计算结果不在合法范围
**【API】**LoadData3DParams.srcStride / channelSize（tiling 驱动）
**【约束】**`T.srcStride × (T.repeat - 1) + T.channelSize × sizeof(element) ≤ A1_buffer_size`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 变量名匹配

## 修复方案

### 标准修复模式

1. **修复模式 1（增大 A1 buffer 分配大小）**：`qA1.InitBuffer(half*, 2048);`（确保 buffer 容量满足 LoadData3D 最大读取需求）
2. **修复模式 2（调整 LoadData3D 读取参数）**：`uint32_t maxRead = A1_BUFFER_SIZE / sizeof(half); uint32_t actualRead = (ldParams.repeat - 1) * ldParams.srcStride + ldParams.channelSize; if (actualRead > maxRead) { ldParams.repeat = (maxRead - ldParams.channelSize) / ldParams.srcStride + 1; }`
3. **修复模式 3（srcStride 合理性校验）**：`uint32_t maxSrcStride = (A1_BUFFER_SIZE / sizeof(half) - ldParams.channelSize) / (ldParams.repeat > 1 ? ldParams.repeat - 1 : 1); ldParams.srcStride = std::min(tiling.srcStride, maxSrcStride);`
4. **修复模式 4（Tiling 参数边界保护）**：channelSize / repeat / srcStride 全部按 A1 容量反向计算上限

### 代码模板

模板：安全的 LoadData3D 读取（detector 原文）

```cpp
template<typename T>
__aicore__ inline void SafeLoadData3DRead(
    LocalTensor<T>& dst,
    LocalTensor<T>& src,
    LoadData3DParams& params,
    uint32_t a1BufferSize)
{
    uint32_t maxReadAddr = (params.repeat - 1) * params.srcStride + params.channelSize;
    if (maxReadAddr * sizeof(T) <= a1BufferSize) {
        LoadData3D(dst, src, params);
    }
}
```

模板：A1 buffer 大小计算（detector 原文）

```cpp
__aicore__ inline uint32_t CalcA1BufferSize(
    uint32_t srcStride,
    uint32_t channelSize,
    uint32_t repeat,
    uint32_t elementSize)
{
    uint32_t maxReadAddr = (repeat - 1) * srcStride + channelSize;
    uint32_t required = maxReadAddr * elementSize;
    uint32_t aligned = (required + 31) & ~31U;
    return aligned;
}
```

### 防御性编程建议

1. **A1 buffer 大小与 LoadData3D 读取量匹配**：InitBuffer 大小 ≥ (repeat-1) × srcStride + channelSize
2. **srcStride 合理性校验**：设置 srcStride 前确保步进后的读取地址仍在 A1 内
3. **Tiling 参数校验**：host 侧计算 tiling 时验证读取相关参数不超出 A1 范围
4. **读取量上限计算**：使用 `(A1_SIZE - channelSize) / srcStride + 1` 计算 repeat 上限
5. **Buffer 大小预留余量**：A1 buffer 分配时适当大于理论最小值，避免边界溢出
6. **单元测试覆盖边界**：对 A1 buffer 恰好满足和略小于读取需求的情况分别测试

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| A1 buffer 过小 | cann-aicore/08_loaddata/ld3d_read_oob_verified | InitBuffer 分配大小不足以提供 LoadData3D 读取所需数据 | EC2 |
| LoadData3D 读取量超出 A1 容量 | 变体（待生成） | 读取参数设置过大 | EC2 |
| srcStride 过大 | 变体（待生成） | srcStride 步进导致读取地址超出 A1 buffer 范围 | EC2 |
| tiling 驱动的读取参数越界 | 变体（待生成） | tiling 参数计算错误导致读取量异常 | EC3 |

## 等价类划分

变异维度：LoadData3D 读取量（(repeat-1) × srcStride + channelSize）相对于 A1 buffer 容量

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | 读取量 ≤ A1_buffer_size | 合法范围 | 读取范围在 A1 内，安全 |
| EC2 | 单次读取量 > A1_buffer_size | A1 buffer 过小或读取参数过大 | 编译期可确定，覆盖原始种子 |
| EC3 | tiling 驱动读取参数越界 | 边界条件下 tiling 计算错误 | 需运行时检查 |

划分依据：MTE 对 LoadData3D 读取 A1 buffer 地址的范围检查。EC2 为编译期可确定的单次超限，EC3 需 host 侧 tiling 计算路径分析。

## 关联异常

- **loaddata3d_write_overflow**：对称异常（推断，非 cross_references 收录）—— loaddata3d_read_overflow 为 LoadData3D 读 A1 越界，loaddata3d_write_overflow 为 LoadData3D 写 A2 越界；同属 08_loaddata 类别 + LoadData3D 族。
- **c0_idx_overflow / fmap_size_illegal / padding_misconfig / first_window_overflow / l1_3d_size_illegal**：共享 LoadData3D API（cross_references 反查 api_index：LoadData3D → [c0_idx_overflow, fmap_size_illegal, padding_misconfig, first_window_overflow, l1_3d_size_illegal]；LoadData → [loaddata2d_read_overflow, loaddata3d_write_overflow, loaddata3d_read_overflow]）。
- **（推断，非 cross_references 收录）**与 loaddata3d_write_overflow 共享 LoadData3D buffer 容量约束机制（loaddata3d_read_overflow = A1 读越界，loaddata3d_write_overflow = A2 写越界）；与 c0_idx_overflow/fmap_size_illegal/padding_misconfig/first_window_overflow/l1_3d_size_illegal 共享 LoadData3D API（同族异常）。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02/03）、变量名匹配（RULE-04）
- **命中判定**：
  - 值域判断类规则——提取 `qA1.InitBuffer(TYPE*, SIZE)` 与 `LoadData3D(DST, A1_BUF, PARAMS)`，计算 `(PARAMS.repeat - 1) × PARAMS.srcStride + PARAMS.channelSize × sizeof(TYPE)` 与 SIZE 比对，超限即命中 RULE-01/02/03；srcStride 单独验证 `srcStride × (repeat - 1) + channelSize × sizeof(element) ≤ A1_buffer_size`；跨行追溯 ≤3 层
  - 变量名匹配类规则——在源码定位 `ldParams.srcStride = tiling.FIELD` 或 `ldParams.channelSize = tiling.FIELD` 表达式，追溯 host 侧 tiling 计算路径；边界条件下参数值验证；追溯受限时标记 `trace_limited=true`
- **修复映射**：
  - loaddata3d_read_overflow-RULE-01（A1 buffer 过小）→ 修复模式 1（增大 buffer）
  - loaddata3d_read_overflow-RULE-02（读取参数超出 A1 容量）→ 修复模式 2（调整读取参数）
  - loaddata3d_read_overflow-RULE-03（srcStride 越界）→ 修复模式 3（srcStride 合理性校验）
  - loaddata3d_read_overflow-RULE-04（tiling 驱动越界）→ 修复模式 4（tiling 参数边界保护）
