# c0_idx_overflow — C0 Index 溢出

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | c0_idx_overflow |
| 异常类型 | mte_cidx_overflow |
| 错误码 | 24 |
| 类别 | loaddata |
| 源文件 | 来源: stable_ez9999 历史种子（legacy: W10） |
| 报错模式 | EZ9999_stderr |
| 内存区域 | L1 |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

在 LoadData 3D 操作中，`mStartPt` 参数被设置为超出 L1 缓冲区容量限制的值。硬件在执行数据加载时，C0 Index 计算溢出，触发 mte_cidx_overflow 异常。历史种子（legacy: W10） 中 A1 buffer 分配为 fmapA1Size(=C1×H×W×C0=1×8×8×16=1024) × sizeof(half)=2048 字节，`l1_3d_size = 64`，而 违规模式 `mStartPt = 200` 使 C0 索引起点超出有效范围。

- 违规模式（历史种子（legacy: W10））：`mStartPt = 200` — C0 索引起点超出 L1 缓冲区容量限制，触发 mte_cidx_overflow（错误码 24）
- 安全模式：`mStartPt = 0` — 正常起点，安全
- 最小触发条件：LoadData3D 的 mStartPt 参数超出 L1 缓冲区容量限制

## 检测规则

### c0_idx_overflow-RULE-01：mStartPt 直接赋值越界

**【条件】**代码中出现 `params.mStartPt = CONSTANT`（如 `mStartPt = 200`），CONSTANT 为编译期常量且超出 L1 容量上限
**【API】**LoadData3D（`mStartPt` 参数）
**【约束】**`mStartPt < L1_CAPACITY`
**【来源】**代码 diff 直接提取
**【匹配模式】** 值域判断

匹配示例（KB）：

- 历史种子（legacy: W10） 中 `mStartPt = 200`，超出 l1_3d_size=64 的有效索引范围

匹配示例（detector 补充）：

```cpp
params.mStartPt = 1024;
params.mStartPt = 0x100000;
```

### c0_idx_overflow-RULE-02：tiling 计算中乘法溢出

**【条件】**`mStartPt = blockIdx.x * tileLength` 或 `mStartPt = blockIdx.y * blockDim.x * elementSize`，由 blockIdx 与 tileSize 乘积计算，可能溢出
**【API】**LoadData3D（经 tiling 计算的 `mStartPt`）
**【约束】**`blockIdx.x × tileSize < L1_CAPACITY`
**【来源】**LLM 推断
**【匹配模式】** 值域判断

匹配示例（detector 补充）：

```cpp
params.mStartPt = blockIdx.x * tileLength;
params.mStartPt = blockIdx.y * blockDim.x * elementSize;
```

### c0_idx_overflow-RULE-03：多维 index 累加越界

**【条件】**`mStartPt = baseOffset + kStartPt * stride` 或 `mStartPt = mOffset + kIdx * channelStride`，多个 index 参数累加后可能超过 L1 容量
**【API】**LoadData3D（多维 index 组合的 `mStartPt`）
**【约束】**`mStartPt + kStartPt × stride < L1_CAPACITY`
**【来源】**LLM 推断
**【匹配模式】** 值域判断

匹配示例（detector 补充）：

```cpp
params.mStartPt = baseOffset + kStartPt * stride;
params.mStartPt = mOffset + kIdx * channelStride;
```

### c0_idx_overflow-RULE-04：L1 边界校验缺失

**【条件】**LoadData3D 调用前无任何 `mStartPt < L1_CAPACITY` 的断言或判断
**【API】**LoadData3D（调用前的边界校验）
**【约束】**LoadData3D 调用前存在 `mStartPt < L1_CAPACITY` 断言或判断
**【来源】**LLM 推断
**【匹配模式】** 存在性检查

### c0_idx_overflow-RULE-05：tiling 参数范围校验缺失

**【条件】**tiling 结构体中缺少 L1 容量相关的校验字段，tiling 参数传递中缺少容量校验
**【API】**tiling 参数结构体
**【约束】**tiling 结构体中存在容量校验字段或逻辑
**【来源】**LLM 推断
**【匹配模式】** 存在性检查

## 修复方案

### 标准修复模式

1. **修复模式 1（mStartPt 边界 clamp）**：`params.mStartPt = std::min(offset, L1_CAPACITY - elementSize);`
2. **修复模式 2（tiling 计算安全除法）**：`uint32_t safeTileLength = std::min(tileLength, L1_CAPACITY / gridDim.x); params.mStartPt = blockIdx.x * safeTileLength;`
3. **修复模式 3（多维 index 累加校验）**：`uint32_t totalIdx = baseOffset + kStartPt * stride; ASSERT(totalIdx < L1_CAPACITY); params.mStartPt = std::min(totalIdx, L1_CAPACITY - elementSize);`
4. **修复模式 4（编译期常量校验）**：`static_assert(M_START_PT < L1_CAPACITY, "mStartPt exceeds L1 capacity");`

### seed 具体修复

- 历史种子（legacy: W10）：`mStartPt = 200` → `mStartPt = 0`（恢复正常起点）

### 代码模板

模板 1：安全 LoadData3D 封装（detector 补充）

```cpp
template<typename T>
inline void SafeLoadData3D(LoadData3DParams& params, uint32_t l1Capacity) {
    constexpr uint32_t elementSize = sizeof(T);
    ASSERT(params.mStartPt + elementSize <= l1Capacity);
    ASSERT(params.kStartPt * elementSize <= l1Capacity);
    uint32_t totalIndex = params.mStartPt + params.kStartPt * params.stride;
    ASSERT(totalIndex < l1Capacity);
    params.mStartPt = std::min(params.mStartPt, l1Capacity - elementSize);
    LoadData3D(params);
}
```

模板 2：tiling 驱动的安全 index 计算（detector 补充）

```cpp
inline uint32_t ComputeSafeMStartPt(uint32_t blockIdx, uint32_t tileLen, uint32_t l1Cap) {
    uint32_t idx = blockIdx * tileLen;
    return idx >= l1Cap ? (l1Cap - tileLen) : idx;
}
```

### 防御性编程建议

1. **始终校验 L1 容量边界**：在设置 `mStartPt` 前确认值严格小于 L1 分配大小，使用 `ASSERT` 或 `static_assert` 进行编译期/运行时校验
2. **tiling 参数安全计算**：在 tiling 模板中加入 `min()` 上限保护，避免 blockIdx × tileSize 在极端情况下超过 L1 容量
3. **多维 index 累加验证**：当 mStartPt 由多个参数累加计算时，在最终赋值前验证总和不超过 L1 上限
4. **参数初始化归零**：确保 LoadData3DParams 在使用前所有 index 相关字段初始化为 0，避免未初始化值导致的随机溢出
5. **运行时日志记录**：在关键路径添加日志，记录 `mStartPt` 和 `kStartPt` 的实际值，便于问题定位
6. **单元测试覆盖边界值**：为 tiling 参数设计 L1 容量边界值的测试用例，确保 index 计算在极端输入下仍安全

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| 历史种子（legacy: W10）（直接迁移） | stable_ez9999 历史种子（legacy: W10） | `mStartPt = 200` 使 C0 索引超出 L1 容量限制 | EC2 |
| mStartPt 过大 | 变体（待生成） | `params.mStartPt = 1024` 直接超出 L1 容量 | EC2 |
| kStartPt 过大 | 变体（待生成） | `kStartPt = 2048` k 维度 index 越界 | EC2 |
| tiling 驱动 index 越界 | 变体（待生成） | `blockIdx.x * tileSize` tiling 计算溢出 | EC2 |
| 多维 index 组合越界 | 变体（待生成） | `mStartPt + kStartPt * stride` 组合累加超上限 | EC2 |

## 等价类划分

变异维度：LoadData3D mStartPt 相对于 L1 容量

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | mStartPt < L1_CAPACITY | 0 ~ L1_CAPACITY-1 | 索引在 L1 容量内，安全 |
| EC2 | mStartPt ≥ L1_CAPACITY | ≥ L1_CAPACITY | 索引超出 L1 容量，触发 mte_cidx_overflow，覆盖 历史种子（legacy: W10） |

划分依据：MTE 对 LoadData3D C0 index 的容量范围检查。

## 关联异常

- **loaddata2d_read_overflow**：共享 L1 buffer 范围约束，loaddata2d_read_overflow 为 srcStride 步进越界，c0_idx_overflow 为 mStartPt 索引越界。
- **l1_3d_size_illegal**：共享 LoadData3D 参数校验机制，l1_3d_size_illegal 为 channelSize 非法，c0_idx_overflow 为 mStartPt 溢出。
- **fmap_size_illegal/padding_misconfig/first_window_overflow**：同属 09_loaddata3d 类别，均涉及 LoadData3D 参数异常。
- **共享参数**：（推断，非 cross_references "参数共享"表收录）与 loaddata2d_read_overflow 共享 L1_CAPACITY 约束；与 fmap_size_illegal/padding_misconfig/first_window_overflow/l1_3d_size_illegal 共享 LoadData3DParams 参数族。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02/03）、存在性检查（RULE-04/05）
- **命中判定**：
  - 值域判断类规则——提取 `params.mStartPt = VALUE` 表达式，VALUE 为编译期常量且 ≥ L1_CAPACITY 即命中；tiling 驱动场景计算 `blockIdx.x × tileSize` 与 L1_CAPACITY 比对；多维场景计算 `baseOffset + kStartPt × stride` 总和；跨行追溯 ≤3 层
  - 存在性检查类规则——全文模式搜索 LoadData3D 调用前是否存在 `mStartPt < L1_CAPACITY` 断言；tiling 结构体中是否存在容量校验字段（缺失即命中）
- **修复映射**：
  - c0_idx_overflow-RULE-01（mStartPt 直接赋值越界）→ 修复模式 1（边界 clamp）/ 修复模式 4（编译期校验）
  - c0_idx_overflow-RULE-02（tiling 乘法溢出）→ 修复模式 2（tiling 安全除法）
  - c0_idx_overflow-RULE-03（多维 index 累加越界）→ 修复模式 3（多维 index 校验）
  - c0_idx_overflow-RULE-04（L1 边界校验缺失）→ 修复模式 1 / 修复模式 4
  - c0_idx_overflow-RULE-05（tiling 参数范围校验缺失）→ 修复模式 2
