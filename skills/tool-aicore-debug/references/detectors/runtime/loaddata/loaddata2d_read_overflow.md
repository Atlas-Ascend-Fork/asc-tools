# loaddata2d_read_overflow — LoadData2D 读越界

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | loaddata2d_read_overflow |
| 异常类型 | mte_read_overflow |
| 错误码 | 42 |
| 类别 | loaddata |
| 源文件 | 来源: stable_ez9999 历史种子（legacy: P1） |
| 报错模式 | EZ9999_stderr |
| 内存区域 | L1 |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

LoadData2D 指令执行时，srcStride 参数设置过大，导致数据读取地址超出 L1 buffer 的实际分配范围。MTE（Memory Transfer Engine）检测到读操作越界后触发溢出异常。srcStride 定义了每次迭代中源地址的步进量，当 srcStride × repeat 超过 L1 buffer 大小时，必然发生越界读取。历史种子（legacy: P1） 中 A1 buffer 分配为 TOTAL_LENGTH(64) × sizeof(half) = 128 字节，而 违规模式 `ldA.srcStride = 128; ldA.repeatTimes = 16` 使读取范围远超此容量。

- 违规模式（历史种子（legacy: P1））：`ldA.repeatTimes = 16; ldA.srcStride = 128; ldA.dstGap = 0; ldA.ifTranspose = false;` — srcStride=128 且 repeatTimes=16，累积读取远超 A1 buffer 的 128 字节分配
- 安全模式：`ldA.repeatTimes = 1; ldA.srcStride = 0; ldA.dstGap = 0; ldA.ifTranspose = false;` — 无步进，单次读取，安全
- 最小触发条件：LoadData2D 的 srcStride 参数设置过大或 srcStride × repeatTimes 超出 L1 buffer 分配范围

## 检测规则

### loaddata2d_read_overflow-RULE-01：srcStride 超出 L1 buffer 范围

**【条件】**代码中出现 `LoadData2DParams` 参数的 `srcStride` 字段被赋值为编译期常量大值（如 `ldParams.srcStride = 128`），且该值超出 SRC buffer 的 InitBuffer 分配大小
**【API】**LoadData2D（`srcStride` 参数）
**【约束】**`srcStride <= l1BufferSize`
**【来源】**代码 diff 直接提取
**【匹配模式】** 值域判断

匹配示例（KB）：

- 历史种子（legacy: P1） 中 `ldA.srcStride = 128`，A1 buffer 分配为 64 × sizeof(half) = 128 字节

匹配示例（detector 补充）：

```cpp
LoadData2DParams ldParams;
ldParams.srcStride = 1024;
```

### loaddata2d_read_overflow-RULE-02：srcStride × repeat 超出 L1

**【条件】**`LoadData2DParams` 的 `srcStride` 单值可能在范围内，但 `srcStride × repeat` 超过 L1 buffer 实际大小（如 `srcStride=128, repeatTimes=16`，乘积 2048 远超 128 字节）
**【API】**LoadData2D（`srcStride` × `repeatTimes` 组合）
**【约束】**`srcStride × repeat <= l1BufferSize`
**【来源】**API 文档约束
**【匹配模式】** 值域判断

匹配示例（KB）：

- 历史种子（legacy: P1） 中 `ldA.srcStride = 128; ldA.repeatTimes = 16`，128 × 16 = 2048 >> 128 字节

### loaddata2d_read_overflow-RULE-03：Tiling 驱动的 srcStride 计算

**【条件】**`ldParams.srcStride = tiling.strideValue` 或类似形式，tiling 参数计算结果可能使 srcStride 超出 L1 buffer 合法范围
**【API】**LoadData2D（经 tiling 参数计算的 `srcStride`）
**【约束】**`tiling.strideValue <= L1_BUFFER_SIZE`
**【来源】**LLM 推断
**【匹配模式】** 值域判断

匹配示例（detector 补充）：

```cpp
ldParams.srcStride = tiling.strideValue;
```

### loaddata2d_read_overflow-RULE-04：累积偏移越界

**【条件】**循环中多次调用 LoadData2D，`offset += ldParams.srcStride` 累积偏移后超出 L1 buffer 范围，缺少上界检查
**【API】**LoadData2D（循环中的累积偏移）
**【约束】**`startOffset + (repeat - 1) × srcStride + elementSize <= L1_BUFFER_SIZE`
**【来源】**LLM 推断
**【匹配模式】** 存在性检查

匹配示例（detector 补充）：

```cpp
for (int i = 0; i < tileNum; i++) {
    LoadData2D(dst, src + offset, ldParams);
    offset += ldParams.srcStride;
}
```

## 修复方案

### 标准修复模式

1. **修复模式 1（调整 srcStride 为合理值）**：将 `srcStride` 设置为不超过 L1 buffer 的合理值，确保单步读取不越界
2. **修复模式 2（确保 srcStride × repeat 不超过 L1）**：`uint32_t maxRepeat = L1_BUFFER_SIZE / ldParams.srcStride; ldParams.repeat = min(ldParams.repeat, maxRepeat);`
3. **修复模式 3（Tiling 参数驱动的 srcStride 修正）**：`uint32_t maxStride = L1_BUFFER_SIZE / ldParams.repeat; ldParams.srcStride = min(tiling.strideValue, maxStride);`
4. **修复模式 4（多次 LoadData 累积偏移保护）**：循环中添加 `if (offset + ldParams.srcStride > L1_BUFFER_SIZE) break;` 守卫条件

### seed 具体修复

- 历史种子（legacy: P1）：`ldA.srcStride = 128; ldA.repeatTimes = 16` → `ldA.srcStride = 0; ldA.repeatTimes = 1`（无步进，单次读取）

### 代码模板

模板 1：安全的 LoadData2D 参数设置（detector 补充）

```cpp
__aicore__ inline void SafeLoadData2DParams(
    LoadData2DParams& params,
    uint32_t srcStride,
    uint32_t repeat,
    uint32_t l1BufferSize)
{
    uint32_t maxStride = l1BufferSize / (repeat > 0 ? repeat : 1);
    params.srcStride = (srcStride <= maxStride) ? srcStride : maxStride;
    uint32_t maxRepeat = l1BufferSize / (srcStride > 0 ? srcStride : 1);
    params.repeat = (repeat <= maxRepeat) ? repeat : maxRepeat;
}
```

模板 2：带边界检查的 LoadData2D 调用（detector 补充）

```cpp
template<typename T>
__aicore__ inline void SafeLoadData2D(
    LocalTensor<T>& dst,
    LocalTensor<T>& src,
    LoadData2DParams& params,
    uint32_t l1BufferSize)
{
    uint32_t totalRead = params.srcStride * params.repeat;
    if (totalRead <= l1BufferSize) {
        LoadData2D(dst, src, params);
    }
}
```

### 防御性编程建议

1. **srcStride 与 L1 大小匹配检查**：设置 srcStride 前计算 srcStride × repeat ≤ L1 buffer 大小
2. **Tiling 参数校验**：host 侧计算 tiling 时验证 stride 相关参数不超出 L1 范围
3. **循环偏移上界检查**：循环中累积偏移时添加 `offset < L1_BUFFER_SIZE` 的守卫条件
4. **参数计算使用安全除法**：计算 repeat 时使用 `(L1_SIZE - offset) / srcStride` 确保不越界
5. **Buffer 大小常量化**：将 L1 buffer 大小定义为常量，方便所有 LoadData 参数计算引用
6. **单元测试覆盖边界**：对 srcStride 在边界值（等于 L1 大小、略大于 L1 大小）的情况进行测试

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| 历史种子（legacy: P1）（直接迁移） | stable_ez9999 历史种子（legacy: P1） | `ldA.srcStride = 128; ldA.repeatTimes = 16` 使 LoadData2D 读超出 A1 buffer | EC2 |
| srcStride 单值过大 | 变体（待生成） | `ldParams.srcStride = 1024` 单步超出 L1 | EC2 |
| srcStride × repeat 越界 | 变体（待生成） | srcStride 合理但乘积超出 L1 | EC2 |
| tiling 驱动 srcStride 越界 | 变体（待生成） | tiling.strideValue 越界 | EC2 |
| 累积偏移越界 | 变体（待生成） | 循环中 offset += srcStride 累积越界 | EC2 |

## 等价类划分

变异维度：LoadData2D srcStride 相对于 L1 buffer 容量

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | srcStride × repeat ≤ L1 buffer 大小 | 0 ~ L1_SIZE | 读取范围在 buffer 内，安全 |
| EC2 | srcStride 为编译期常量大值且 × repeat 超出 L1 | 如 128（L1_SIZE=128B 时 128×16=2048 溢出） | 编译期可确定溢出，覆盖 历史种子（legacy: P1） |

划分依据：MTE 对 L1 buffer 访问地址的范围检查。

## 关联异常

- **tiling_overflow**：共享"参数过大导致搬运越界"模式，tiling_overflow 为 Tiling copyBytes 驱动 UB 溢出，loaddata2d_read_overflow 为 srcStride 驱动 L1 溢出。
- **c0_idx_overflow**：共享 L1 buffer 范围约束，c0_idx_overflow 为 mStartPt 索引越界，loaddata2d_read_overflow 为 srcStride 步进越界。
- **loaddata3d_write_overflow/loaddata3d_read_overflow**：同属 08_loaddata 类别（LoadData 越界），loaddata3d_write_overflow/loaddata3d_read_overflow 为 LoadData3D 写/读越界，loaddata2d_read_overflow 为 LoadData2D 读越界。
- **共享参数**：（推断，非 cross_references "参数共享"表收录）与 fixpipe_l1_overflow/fixpipe_srcstride_overflow 共享 srcStride 参数族（cross_references 中 srcStride 条目为 [fixpipe_l1_overflow, fixpipe_srcstride_overflow]，loaddata2d_read_overflow 不在此条目中，经 loaddata2d_read_overflow-RULE-01 srcStride 字段语义关联）。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02/03）、存在性检查（RULE-04）
- **命中判定**：
  - 值域判断类规则——提取 `ldParams.srcStride = VALUE` 表达式，VALUE 为编译期常量且 > l1BufferSize 即命中；同时读 repeatTimes 验证 `srcStride × repeat <= l1BufferSize`；tiling 驱动场景定位 `tiling.strideValue` 变量并追溯赋值；跨行追溯 ≤3 层
  - 存在性检查类规则——全文模式搜索循环结构中 `offset += ldParams.srcStride` 累积偏移，验证是否存在 `offset < L1_BUFFER_SIZE` 守卫（缺失即命中）
- **修复映射**：
  - loaddata2d_read_overflow-RULE-01（srcStride 单值过大）→ 修复模式 1（调整 srcStride）
  - loaddata2d_read_overflow-RULE-02（srcStride × repeat 超限）→ 修复模式 2（确保乘积不超限）
  - loaddata2d_read_overflow-RULE-03（Tiling 驱动）→ 修复模式 3（Tiling 修正）
  - loaddata2d_read_overflow-RULE-04（累积偏移越界）→ 修复模式 4（累积偏移保护）
