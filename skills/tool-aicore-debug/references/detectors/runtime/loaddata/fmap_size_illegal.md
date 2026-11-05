# fmap_size_illegal — fmap 尺寸非法

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | fmap_size_illegal |
| 异常类型 | mte_illegal_fm_size |
| 错误码 | 35 |
| 类别 | loaddata |
| 源文件 | cann-aicore/09_loaddata3d/ld3d_fmap_illegal_verified/error/main.asc |
| 报错模式 | EZ9999_stderr |
| 内存区域 | L1（LoadData3D 作用于 L1 buffer，fmap 尺寸校验） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

在 LoadData 3D 操作中，fmap 的 H 或 W 维度被设置为 0。硬件无法处理尺寸为 0 的 fmap，触发 mte_illegal_fm_size 异常（错误码 35），导致数据加载失败。

触发条件（detector 原文，逐字保留）：

- C1：`fmapInfo.h == 0`（fmap 高度为 0）
- C2：`fmapInfo.w == 0`（fmap 宽度为 0）
- C3：`fmapInfo.h == 0 && fmapInfo.w == 0`（H 和 W 同时为 0）
- C4：tiling 计算中 `H / divisor == 0`（tiling 驱动的 fmap 尺寸计算为 0）

形式化规则前提：`ASSERT_fmap_size_illegal_1: ∀ LoadData3D 调用: params.fmapInfo.h >= 1`；`ASSERT_fmap_size_illegal_2: params.fmapInfo.w >= 1`。

## 检测规则

### fmap_size_illegal-RULE-01：fmapInfo 维度直接赋值为 0

**【条件】**代码中出现 `fmapInfo.(h|w) = 0`，fmap 的 H 或 W 直接赋值为 0
**【API】**LoadData3DParams.fmapInfo.h / w 赋值（字面量 0）
**【约束】**`fmapInfo.h >= 1 ∧ fmapInfo.w >= 1`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
params.fmapInfo.h = 0;
params.fmapInfo.w = 0;
```

### fmap_size_illegal-RULE-02：tiling 除法可能导致结果为 0

**【条件】**`fmapInfo.(h|w) = EXPR / (tileH|tileW|divisor)`，整数除法在输入较小时结果可能为 0
**【API】**LoadData3DParams.fmapInfo.h / w 赋值（除法表达式）
**【约束】**除法结果 ≥ 1（除数 ≤ 被除数）
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
params.fmapInfo.h = totalH / tileH;
params.fmapInfo.w = inputW / blockW;
```

### fmap_size_illegal-RULE-03：动态 shape 未做最小值保护

**【条件】**`fmapInfo.(h|w) = inputShape[..] / dims.FIELD`，fmap 尺寸来自动态 shape 输入，缺少最小值校验
**【API】**LoadData3DParams.fmapInfo.h / w 赋值（动态 shape）
**【约束】**`fmapInfo.h = max(shape_value, 1) ∧ fmapInfo.w = max(shape_value, 1)`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 变量名匹配

匹配示例（detector 原文）：

```cpp
params.fmapInfo.h = inputShape[2];
params.fmapInfo.w = dims.width;
```

### fmap_size_illegal-RULE-04：fmap 尺寸合法校验缺失（运行时检测）

**【条件】**LoadData3D 调用前不存在 `h > 0 && w > 0` 断言
**【API】**LoadData3D（调用前合法性校验）
**【约束】**LoadData3D 调用前存在 `ASSERT(fmapInfo.h > 0 && fmapInfo.w > 0)`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

### fmap_size_illegal-RULE-05：tiling 计算边界保护（运行时检测）

**【条件】**tiling 参数中 divisor 大于被除数（tileH > totalH 或 tileW > totalW），除法结果为 0
**【API】**tiling 计算（divisor 与被除数关系）
**【约束】**`max(totalH / tileH, 1) >= 1 ∧ max(totalW / tileW, 1) >= 1`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

## 修复方案

### 标准修复模式

1. **修复模式 1（最小值保护）**：`params.fmapInfo.h = std::max(totalH / tileH, 1u); params.fmapInfo.w = std::max(totalW / tileW, 1u);`
2. **修复模式 2（向上取整除法）**：`auto ceilDiv = [](uint32_t a, uint32_t b) { return (a + b - 1) / b; }; params.fmapInfo.h = std::max(ceilDiv(inputH, blockDim.y), 1u);`
3. **修复模式 3（动态 shape 校验）**：`ASSERT(rawH > 0 && rawW > 0); params.fmapInfo.h = rawH;`
4. **修复模式 4（编译期常量校验）**：`constexpr uint32_t FMAP_H = 16; static_assert(FMAP_H > 0, "fmap H must be positive");`

### 代码模板

模板 1：安全 fmap 尺寸计算（detector 原文）

```cpp
inline FmapInfo SafeFmapInfo(uint32_t h, uint32_t w) {
    ASSERT(h > 0 && w > 0);
    return FmapInfo{.h = std::max(h, 1u), .w = std::max(w, 1u)};
}
```

模板 2：tiling 驱动的安全 fmap 计算（detector 原文）

```cpp
inline FmapInfo SafeTilingFmap(uint32_t totalH, uint32_t totalW, uint32_t tileH, uint32_t tileW) {
    uint32_t h = tileH > 0 ? std::max(totalH / tileH, 1u) : 1u;
    uint32_t w = tileW > 0 ? std::max(totalW / tileW, 1u) : 1u;
    return FmapInfo{.h = h, .w = w};
}
```

### 防御性编程建议

1. **始终校验 fmap 尺寸非零**：在设置 `fmapInfo.h` 和 `fmapInfo.w` 前加入 `ASSERT(h > 0 && w > 0)` 确保合法性。
2. **使用向上取整除法**：在 tiling 计算中优先使用 `ceil_div` 或 `(a + b - 1) / b` 替代直接整数除法，避免结果为 0。
3. **最小值保护**：对所有 fmap 尺寸计算结果使用 `max(result, 1)` 保护，确保硬件始终获得合法尺寸。
4. **动态 shape 前置校验**：在 kernel 入口处校验动态 shape 的 H 和 W 维度，对非法输入直接返回错误。
5. **单元测试覆盖零值**：设计 H=1、W=1 的边界测试用例，以及 divisor > dividend 的极端场景。
6. **tiling 参数联动校验**：确保 tileH 和 tileW 不超过 totalH 和 totalW，避免除法结果为 0。

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| H=0 | cann-aicore/09_loaddata3d/ld3d_fmap_illegal_verified | `fmapInfo.h = 0`，高度维度为 0 | EC2 |
| W=0 | 变体（待生成） | `fmapInfo.w = 0`，宽度维度为 0 | EC2 |
| H 和 W 同时为 0 | 变体（待生成） | `fmapInfo.h = 0, fmapInfo.w = 0`，双维度同时非法 | EC2 |
| tiling 驱动的 fmap 尺寸计算为 0 | 变体（待生成） | `H / tileH = 0`，tiling 除法结果为 0 | EC3 |

## 等价类划分

变异维度：LoadData3DParams.fmapInfo.h/w 是否 ≥ 1

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | h ≥ 1 ∧ w ≥ 1 | 合法范围 | fmap 尺寸合法，安全 |
| EC2 | h 或 w 直接赋值为 0 | = 0 | 编译期可确定，覆盖原始种子 |
| EC3 | tiling 除法结果为 0 | divisor > dividend | 需运行时检查 |

划分依据：硬件对 fmap 尺寸的合法性检查。EC2 为编译期可确定的字面量 0，EC3 需 host 侧 tiling 计算路径分析。

## 关联异常

- **padding_misconfig / first_window_overflow / l1_3d_size_illegal**：同属 fmap 族 + 09_loaddata3d 类别（cross_references 反查 keyword_index：fmap → [fmap_size_illegal]，padding → [padding_misconfig]，mte_f1wpos_larger_fsize → [first_window_overflow]，mte_illegal_l1_3d_size → [l1_3d_size_illegal]）。共享 LoadData3D API 与 fmap 概念。
- **c0_idx_overflow**：同属 LoadData3D 族（cross_references 反查 api_index：LoadData3D → [c0_idx_overflow, fmap_size_illegal, padding_misconfig, first_window_overflow, l1_3d_size_illegal]）。c0_idx_overflow 为 C0 Index 溢出，fmap_size_illegal 为 fmap 尺寸非法。
- **LoadData3D / fmap / mte_illegal_fm_size**：共享 API + keyword（cross_references 反查：LoadData3D → [c0_idx_overflow..l1_3d_size_illegal]，fmap → [fmap_size_illegal]，mte_illegal_fm_size → [fmap_size_illegal]）。
- **（推断，非 cross_references 收录）**与 l1_3d_size_illegal 共享 LoadData3D 配置参数非法语义（fmap_size_illegal = fmap H/W 为 0，l1_3d_size_illegal = channelSize 为 0），但参数字段不同；与 fixpipe_cfg_illegal/mmad_illegal 共享"参数为 0 触发非法配置"语义模式。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02/05）、变量名匹配（RULE-03）、存在性检查（RULE-04）
- **命中判定**：
  - 值域判断类规则——提取 `params.fmapInfo.(h|w) = EXPR` 表达式，EXPR 为字面量 0 即命中 RULE-01；EXPR 为除法表达式时追溯除数与被除数关系（除数 > 被除数即命中 RULE-02/05）；跨行追溯 ≤3 层
  - 变量名匹配类规则——在源码定位 `params.fmapInfo.(h|w) = inputShape[..]` 或 `dims.FIELD` 表达式，追溯动态 shape 输入；追溯受限时标记 `trace_limited=true`
  - 存在性检查类规则——LoadData3D 调用前是否存在 `h > 0 && w > 0` 断言（缺失即命中 RULE-04）
- **修复映射**：
  - fmap_size_illegal-RULE-01（维度直接为 0）→ 修复模式 1（最小值保护）
  - fmap_size_illegal-RULE-02（tiling 除法为 0）→ 修复模式 2（向上取整除法）/ 修复模式 1
  - fmap_size_illegal-RULE-03（动态 shape 未保护）→ 修复模式 3（动态 shape 校验）
  - fmap_size_illegal-RULE-04（合法性校验缺失）→ 修复模式 1（添加 ASSERT）
  - fmap_size_illegal-RULE-05（tiling 边界保护）→ 修复模式 2
