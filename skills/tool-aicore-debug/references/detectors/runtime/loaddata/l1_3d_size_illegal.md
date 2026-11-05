# l1_3d_size_illegal — L1 3D Size 非法

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | l1_3d_size_illegal |
| 异常类型 | mte_illegal_l1_3d_size |
| 错误码 | 36 |
| 类别 | loaddata |
| 源文件 | cann-aicore/09_loaddata3d/ld3d_l1size_illegal_verified/error/main.asc |
| 报错模式 | EZ9999_stderr |
| 内存区域 | L1（LoadData3D channelSize 影响 L1 3D buffer 分配） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

在 LoadData 3D 操作中，`channelSize` 被设置为 0 或过小的非法值。硬件无法根据该参数分配有效的 L1 3D 缓冲区空间，触发 mte_illegal_l1_3d_size 异常（错误码 36），数据加载失败。

触发条件（detector 原文，逐字保留）：

- C1：`channelSize == 0`（channel 尺寸为 0）
- C2：`channelSize < minAlign`（channelSize 小于对齐要求）
- C3：tiling 计算的 channelSize 异常（tiling 参数推导错误）
- C4：channelSize 与其他参数不匹配（channelSize 与 fmap/filter 参数矛盾）

形式化规则前提：`ASSERT_l1_3d_size_illegal_1: ∀ LoadData3D 调用: params.channelSize >= 1`；`ASSERT_l1_3d_size_illegal_2: params.channelSize % C0_ALIGN == 0`。

## 检测规则

### l1_3d_size_illegal-RULE-01：channelSize 直接赋值为 0

**【条件】**代码中出现 `channelSize = 0`，channelSize 直接设置为 0
**【API】**LoadData3DParams.channelSize 赋值（字面量 0）
**【约束】**`channelSize >= 1`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
params.channelSize = 0;
```

### l1_3d_size_illegal-RULE-02：tiling 计算中 channelSize 推导为 0

**【条件】**`channelSize = tiling.channel / blockDim.x` 或 `channels / groupNum`，channelSize 由 tiling 参数计算，可能结果为 0
**【API】**LoadData3DParams.channelSize 赋值（tiling 除法表达式）
**【约束】**`tiling.channel > 0 ⇒ channelSize > 0`，除法结果 ≥ 1
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
params.channelSize = tiling.channel / blockDim.x;
params.channelSize = channels / groupNum;
```

### l1_3d_size_illegal-RULE-03：channelSize 缺少对齐校验

**【条件】**`channelSize = rawChannels × sizeof(float)` 后无 `% C0_ALIGN == 0` 校验，channelSize 可能不满足硬件对齐要求
**【API】**LoadData3DParams.channelSize 赋值（无对齐校验）
**【约束】**`channelSize % C0_ALIGN == 0`（通常 C0_ALIGN = 32 bytes）
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

匹配示例（detector 原文）：

```cpp
params.channelSize = rawChannels * sizeof(float);
```

### l1_3d_size_illegal-RULE-04：channelSize 非零与对齐校验缺失（运行时检测）

**【条件】**LoadData3D 调用前缺少 `channelSize > 0 && channelSize % C0 == 0` 断言
**【API】**LoadData3D（调用前 channelSize 合法性校验）
**【约束】**LoadData3D 调用前存在 `ASSERT(channelSize > 0 && channelSize % C0 == 0)`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

### l1_3d_size_illegal-RULE-05：channelSize 与数据类型匹配（运行时检测）

**【条件】**channelSize 不是 sizeof(T) 的整数倍，channelSize 与数据类型大小不匹配
**【API】**LoadData3DParams.channelSize × sizeof(elementType)
**【约束】**`channelSize >= sizeof(elementType) ∧ channelSize % sizeof(elementType) == 0`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

## 修复方案

### 标准修复模式

1. **修复模式 1（channelSize 最小值保护）**：`constexpr uint32_t C0_ALIGN = 32; params.channelSize = std::max(tiling.channel, C0_ALIGN);`
2. **修复模式 2（对齐向上取整）**：`uint32_t rawSize = channels × sizeof(float); params.channelSize = ((rawSize + C0_ALIGN - 1) / C0_ALIGN) × C0_ALIGN; ASSERT(params.channelSize % C0_ALIGN == 0);`
3. **修复模式 3（tiling 除法安全计算）**：`uint32_t perGroup = totalChannels / std::max(groupNum, 1u); params.channelSize = std::max(((perGroup + C0_ALIGN - 1) / C0_ALIGN) × C0_ALIGN, C0_ALIGN);`
4. **修复模式 4（数据类型匹配校验）**：`ASSERT(rawChannelBytes % sizeof(float) == 0); params.channelSize = std::max(rawChannelBytes, C0_ALIGN);`

### 代码模板

模板 1：安全 channelSize 计算（detector 原文）

```cpp
template<typename T>
inline uint32_t SafeChannelSize(uint32_t channels) {
    constexpr uint32_t C0_ALIGN = 32;
    uint32_t rawSize = channels * sizeof(T);
    uint32_t aligned = ((rawSize + C0_ALIGN - 1) / C0_ALIGN) * C0_ALIGN;
    return std::max(aligned, C0_ALIGN);
}
```

模板 2：channelSize 校验器（detector 原文）

```cpp
inline void ValidateChannelSize(uint32_t channelSize, uint32_t elementSize) {
    constexpr uint32_t C0_ALIGN = 32;
    ASSERT(channelSize > 0);
    ASSERT(channelSize % C0_ALIGN == 0);
    ASSERT(channelSize >= elementSize);
}
```

### 防御性编程建议

1. **始终校验 channelSize 非零**：在 LoadData3D 调用前使用 `ASSERT(channelSize > 0)` 确保合法性。
2. **确保对齐要求**：channelSize 必须满足 C0 对齐（通常为 32 bytes），使用向上取整计算。
3. **tiling 除法保护**：当 channelSize 由除法计算时，确保除数不为 0 且结果不为 0。
4. **与数据类型联动校验**：channelSize 应为 `sizeof(elementType)` 的整数倍。
5. **参数传递一致性**：确保 tiling 结构体中的 channel 字段与实际 channelSize 一致。
6. **单元测试覆盖零值与边界**：测试 channels=0、channels=1 等边界场景，确保不会触发硬件异常。

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| channelSize=0 | cann-aicore/09_loaddata3d/ld3d_l1size_illegal_verified | `channelSize = 0`，直接赋值为 0 | EC2 |
| channelSize 过小 | 变体（待生成） | `channelSize = 1`，小于对齐要求 | EC2 |
| tiling 驱动的 channelSize 异常 | 变体（待生成） | `tiling.channel = 0`，tiling 计算结果为 0 | EC3 |
| channelSize 与其他参数不匹配 | 变体（待生成） | `channelSize ≠ actual × sizeof(T)`，类型与大小不匹配 | EC2 |

## 等价类划分

变异维度：LoadData3DParams.channelSize 是否 ≥ 1 且满足 C0 对齐

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | channelSize >= 1 ∧ channelSize % C0 == 0 | 合法对齐范围 | 安全 |
| EC2 | channelSize = 0 或过小（< C0_ALIGN） | 0 或 < 32 | 编译期可确定，覆盖原始种子 |
| EC3 | tiling 驱动的 channelSize 异常 | tiling 计算结果为 0 | 需运行时检查 |

划分依据：硬件对 LoadData3D channelSize 的合法性与 C0 对齐检查。EC2 为编译期可确定的字面量 0 或过小值，EC3 需 host 侧 tiling 计算路径分析。

## 关联异常

- **fmap_size_illegal / padding_misconfig / first_window_overflow**：同属 fmap 族 + 09_loaddata3d 类别（cross_references 反查 keyword_index：mte_illegal_l1_3d_size → [l1_3d_size_illegal]）。
- **c0_idx_overflow**：同属 LoadData3D 族（cross_references 反查 api_index：LoadData3D → [c0_idx_overflow..l1_3d_size_illegal]）。
- **LoadData3D / mte_illegal_l1_3d_size**：共享 API + keyword（cross_references 反查：LoadData3D → [c0_idx_overflow..l1_3d_size_illegal]，mte_illegal_l1_3d_size → [l1_3d_size_illegal]）。
- **（推断，非 cross_references 收录）**与 fmap_size_illegal 共享 LoadData3D 配置参数非法语义（l1_3d_size_illegal = channelSize 为 0，fmap_size_illegal = fmap H/W 为 0），但参数字段不同；与 fixpipe_cfg_illegal/mmad_illegal 共享"参数为 0 触发非法配置"语义模式。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02/05）、存在性检查（RULE-03/04）
- **命中判定**：
  - 值域判断类规则——提取 `params.channelSize = EXPR` 表达式，EXPR 为字面量 0 或过小值（< C0_ALIGN）即命中 RULE-01；EXPR 为除法表达式时追溯除数与被除数关系（除数 > 被除数即命中 RULE-02）；channelSize 与 sizeof(elementType) 整除关系失败即命中 RULE-05；跨行追溯 ≤3 层
  - 存在性检查类规则——channelSize 赋值后是否存在 `% C0_ALIGN == 0` 对齐校验（缺失即命中 RULE-03）；LoadData3D 调用前是否存在 `channelSize > 0 && channelSize % C0 == 0` 断言（缺失即命中 RULE-04）
- **修复映射**：
  - l1_3d_size_illegal-RULE-01（channelSize 直接为 0）→ 修复模式 1（最小值保护）
  - l1_3d_size_illegal-RULE-02（tiling 除法为 0）→ 修复模式 3（tiling 除法安全计算）
  - l1_3d_size_illegal-RULE-03（缺少对齐校验）→ 修复模式 2（对齐向上取整）
  - l1_3d_size_illegal-RULE-04（非零与对齐校验缺失）→ 修复模式 2 / 模板 2（channelSize 校验器）
  - l1_3d_size_illegal-RULE-05（数据类型匹配）→ 修复模式 4（数据类型匹配校验）
