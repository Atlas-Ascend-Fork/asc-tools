# loaddata3d_write_overflow — LoadData 3D 写越界

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | loaddata3d_write_overflow |
| 异常类型 | mte_write_3d_overflow |
| 错误码 | 47 |
| 类别 | loaddata |
| 源文件 | cann-aicore/08_loaddata/ld3d_write_oob_verified/error/main.asc |
| 报错模式 | EZ9999_stderr |
| 内存区域 | L0_A（LoadData3D 写入 A2 buffer 越界；A2 属 L0A 侧） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

LoadData3D 指令执行时，目标 A2 buffer 的分配大小不足以容纳写入的数据量。当通过 `qA2.InitBuffer` 分配的 A2 buffer 过小（如仅分配 64 字节），而 LoadData3D 的写入参数（如 channelSize、repeat）要求的写入量超过 buffer 容量时，MTE 检测到写操作越界并触发 mte_write_3d_overflow 异常（错误码 47）。

触发条件（detector 原文，逐字保留）：

- A2 buffer 的 InitBuffer 分配大小过小（如仅 64 字节）
- LoadData3D 的 channelSize 过大导致单次写入超出 A2
- LoadData3D 的 repeat 参数导致累积写入超出 A2 容量
- 多次 LoadData 操作累积写入量超出 A2 buffer

形式化规则前提：`ASSERT loaddata3d_write_overflow-BUF-01: ∀ LoadData3D 调用: write_size = PARAMS.channelSize × PARAMS.repeat × sizeof(element) ≤ a2_size`。

## 检测规则

### loaddata3d_write_overflow-RULE-01：A2 buffer 分配过小

**【条件】**`qA2.InitBuffer(TYPE*, SIZE)` 分配大小 SIZE，`LoadData3D(A2_BUF, SRC, PARAMS)` 写入量超过 SIZE
**【API】**qA2.InitBuffer 分配大小 × LoadData3D 写入量
**【约束】**`SIZE ≥ LoadData3D 最大写入量`（基于 channelSize、repeat 等参数）
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
qA2.InitBuffer(half*, 64);
LoadData3D(a2, a1d, ldParams);
```

### loaddata3d_write_overflow-RULE-02：LoadData3D 参数导致写入超出 A2

**【条件】**`ldParams.channelSize = CHANNEL; ldParams.repeat = REPEAT;`，`channelSize × repeat × sizeof(element)` 总写入量超出 A2 buffer 的 InitBuffer 大小
**【API】**LoadData3DParams.channelSize × repeat × sizeof(element)
**【约束】**`channelSize × repeat × sizeof(element) ≤ A2_buffer_size`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

### loaddata3d_write_overflow-RULE-03：多次 LoadData 累积写越界

**【条件】**对同一 A2 buffer 的多次 LoadData3D 操作累积写入量超出 A2 buffer 容量
**【API】**LoadData3D（多次调用同一 buffer 累积写入）
**【约束】**`SUM(write_size_i) ≤ A2_buffer_size`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

匹配示例（detector 原文）：

```cpp
LoadData3D(a2, src1, params1);
LoadData3D(a2, src2, params2);
```

### loaddata3d_write_overflow-RULE-04：channelSize 动态参数校验（运行时检测）

**【条件】**tiling 驱动的 channelSize 参数可能超出 A2 容量
**【API】**LoadData3DParams.channelSize（tiling 驱动）
**【约束】**`channelSize × sizeof(T) ≤ A2_buffer_size ∧ repeat × channelSize × sizeof(element) ≤ A2_buffer_size`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 变量名匹配

## 修复方案

### 标准修复模式

1. **修复模式 1（增大 A2 buffer 分配大小）**：`qA2.InitBuffer(half*, 2048);`（确保 buffer 容量满足 LoadData3D 最大写入需求）
2. **修复模式 2（调整 LoadData3D 写入参数）**：`uint32_t maxChannel = A2_BUFFER_SIZE / ldParams.repeat; ldParams.channelSize = std::min(ldParams.channelSize, maxChannel);`
3. **修复模式 3（channelSize 动态限制）**：`uint32_t maxChannelSize = A2_BUFFER_SIZE / sizeof(half); ldParams.channelSize = std::min(tiling.channelSize, maxChannelSize);`
4. **修复模式 4（多次 LoadData 累积写入保护）**：循环中添加 `if (totalWritten + writeSize > A2_BUFFER_SIZE) break;` 守卫

### 代码模板

模板：安全的 LoadData3D 写入（detector 原文）

```cpp
template<typename T>
__aicore__ inline void SafeLoadData3DWrite(
    LocalTensor<T>& dst,
    LocalTensor<T>& src,
    LoadData3DParams& params,
    uint32_t a2BufferSize)
{
    uint32_t writeSize = params.channelSize * params.repeat * sizeof(T);
    if (writeSize <= a2BufferSize) {
        LoadData3D(dst, src, params);
    }
}
```

模板：A2 buffer 大小计算（detector 原文）

```cpp
__aicore__ inline uint32_t CalcA2BufferSize(
    uint32_t channelSize,
    uint32_t repeat,
    uint32_t elementSize)
{
    uint32_t required = channelSize * repeat * elementSize;
    uint32_t aligned = (required + 31) & ~31U;
    return aligned;
}
```

### 防御性编程建议

1. **A2 buffer 大小与 LoadData3D 参数匹配**：InitBuffer 大小 ≥ channelSize × repeat × sizeof(T)
2. **ChannelSize 上界校验**：设置 channelSize 前计算是否超出 A2 buffer 容量
3. **Tiling 参数影响评估**：host 侧计算 tiling 时考虑 A2 buffer 的容量限制
4. **累积写入计数**：多次 LoadData3D 操作时跟踪总写入量，不超过 buffer 大小
5. **Buffer 大小预留余量**：InitBuffer 分配时适当预留余量，避免边界条件下的微小溢出
6. **静态分析工具辅助**：使用静态分析工具检查 LoadData3D 写入量与 buffer 大小的关系

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| A2 buffer 过小 | cann-aicore/08_loaddata/ld3d_write_oob_verified | InitBuffer 分配大小不足以容纳 LoadData3D 写入数据 | EC2 |
| LoadData3D 参数导致写入超出 A2 | 变体（待生成） | channelSize 或 repeat 参数设置过大 | EC2 |
| channelSize 过大 | 变体（待生成） | 单通道写入量超出 A2 buffer 容量 | EC2 |
| 多次 LoadData 累积写越界 | 变体（待生成） | 多次 LoadData3D 操作的累积写入超出 A2 范围 | EC3 |

## 等价类划分

变异维度：LoadData3D 写入量相对于 A2 buffer 容量

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | 单次/累积写入量 ≤ A2_buffer_size | 合法范围 | 写入范围在 A2 内，安全 |
| EC2 | 单次写入量 > A2_buffer_size | channelSize × repeat 超限 | 编译期可确定，覆盖原始种子 |
| EC3 | 多次 LoadData 累积写入超限 | SUM(write_size_i) > A2_size | 需运行时分析 |

划分依据：MTE 对 LoadData3D 写入 A2 buffer 地址的范围检查。EC2 为编译期可确定的单次超限，EC3 需多次调用累积分析。

## 关联异常

- **loaddata3d_read_overflow**：对称异常（推断，非 cross_references 收录）—— loaddata3d_write_overflow 为 LoadData3D 写 A2 越界，loaddata3d_read_overflow 为 LoadData3D 读 A1 越界；同属 08_loaddata 类别 + LoadData3D 族。
- **c0_idx_overflow / fmap_size_illegal / padding_misconfig / first_window_overflow / l1_3d_size_illegal**：共享 LoadData3D API（cross_references 反查 api_index：LoadData3D → [c0_idx_overflow, fmap_size_illegal, padding_misconfig, first_window_overflow, l1_3d_size_illegal]；LoadData → [loaddata2d_read_overflow, loaddata3d_write_overflow, loaddata3d_read_overflow]）。loaddata3d_write_overflow/loaddata3d_read_overflow 在 LoadData 条目中，c0_idx_overflow/fmap_size_illegal/padding_misconfig/first_window_overflow/l1_3d_size_illegal 在 LoadData3D 条目中（推断：API 名称归并差异）。
- **（推断，非 cross_references 收录）**与 loaddata3d_read_overflow 共享 LoadData3D buffer 容量约束机制（loaddata3d_write_overflow = A2 写越界，loaddata3d_read_overflow = A1 读越界）；与 c0_idx_overflow/fmap_size_illegal/padding_misconfig/first_window_overflow/l1_3d_size_illegal 共享 LoadData3D API（同族异常）。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02）、API 调用模式（RULE-03）、变量名匹配（RULE-04）
- **命中判定**：
  - 值域判断类规则——提取 `qA2.InitBuffer(TYPE*, SIZE)` 与 `LoadData3D(a2, a1d, ldParams)`，计算 `ldParams.channelSize × ldParams.repeat × sizeof(TYPE)` 与 SIZE 比对，超限即命中 RULE-01/02；跨行追溯 ≤3 层
  - API 调用模式类规则——按 A2 buffer 实例聚合多次 LoadData3D 调用的累积写入量（含写入偏移和重叠情况），与 A2_buffer_size 比对；CFG 跨函数追溯受限时标记 `trace_limited=true`
  - 变量名匹配类规则——在源码定位 `ldParams.channelSize = tiling.FIELD` 表达式，追溯 host 侧 tiling 计算；边界条件下 channelSize 计算结果验证
- **修复映射**：
  - loaddata3d_write_overflow-RULE-01（A2 buffer 过小）→ 修复模式 1（增大 buffer）
  - loaddata3d_write_overflow-RULE-02（参数导致写入超出）→ 修复模式 2（调整写入参数）
  - loaddata3d_write_overflow-RULE-03（多次累积写越界）→ 修复模式 4（累积写入保护）
  - loaddata3d_write_overflow-RULE-04（channelSize 动态校验）→ 修复模式 3（channelSize 动态限制）
