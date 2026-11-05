# first_window_overflow — 首窗口超限

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | first_window_overflow |
| 异常类型 | mte_f1wpos_larger_fsize |
| 错误码 | 26 |
| 类别 | loaddata |
| 源文件 | cann-aicore/09_loaddata3d/ld3d_window_overrun_verified/error/main.asc |
| 报错模式 | EZ9999_stderr |
| 内存区域 | L1（LoadData3D 首窗口位置作用于 L1 buffer） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

在 LoadData 3D 操作中，`kStartPt` 或 `mStartPt` 被设置为过大值，导致首窗口的起始位置超出了 fmap 的实际尺寸。硬件检测到窗口位置越界，触发 mte_f1wpos_larger_fsize 异常（错误码 26），数据加载失败。历史种子（legacy: first_window_overflow） 预期触发 mte_f1wpos_larger_fsize，但实际触发了 ADDR_MISALIGN（0x10000）——首窗口起点越界在硬件上表现为地址非对齐。

触发条件（detector 原文，逐字保留）：

- C1：`kStartPt >= fmapH`（k 维度起始位置超出 fmap 高度）
- C2：`mStartPt >= fmapW`（m 维度起始位置超出 fmap 宽度）
- C3：`kStartPt >= fmapH && mStartPt >= fmapW`（两个维度同时越界）
- C4：tiling 驱动的窗口位置计算越界（tiling 计算导致位置超过 fmap 尺寸）

形式化规则前提：`ASSERT_first_window_overflow_1: params.kStartPt < params.fmapInfo.h`；`ASSERT_first_window_overflow_3: params.kStartPt + params.filterH <= params.fmapInfo.h`。

## 检测规则

### first_window_overflow-RULE-01：kStartPt/mStartPt 常量赋值越界

**【条件】**`(kStartPt|mStartPt) = LARGE_VALUE`（如 1024、2048）且值明显大于常见 fmap 尺寸
**【API】**LoadData3DParams.kStartPt / mStartPt 赋值（字面量大值）
**【约束】**`kStartPt < fmapInfo.h ∧ mStartPt < fmapInfo.w`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
params.kStartPt = 1024;
params.mStartPt = 2048;
```

### first_window_overflow-RULE-02：tiling 计算中窗口位置溢出

**【条件】**`(kStartPt|mStartPt) = blockIdx.x × tileLen` 或 `blockIdx.x × tileW + threadIdx.x`，窗口位置由 blockIdx 乘积计算，可能超过 fmap 尺寸
**【API】**LoadData3DParams.kStartPt / mStartPt 赋值（blockIdx 乘积）
**【约束】**`blockIdx × tileSize < fmapSize`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 变量名匹配

匹配示例（detector 原文）：

```cpp
params.kStartPt = blockIdx.y * tileH;
params.mStartPt = blockIdx.x * tileW + threadIdx.x;
```

### first_window_overflow-RULE-03：窗口位置缺少 fmap 边界校验

**【条件】**`(kStartPt|mStartPt) = EXPR1 + EXPR2` 后无 `min` 或 `ASSERT` 保护，窗口位置由表达式计算但无边界保护
**【API】**LoadData3DParams.kStartPt / mStartPt 赋值（表达式计算）
**【约束】**赋值后存在 `min(value, fmapSize - filterSize)` clamp 或 `ASSERT(kStartPt < fmapSize)` 边界校验
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

匹配示例（detector 原文）：

```cpp
params.kStartPt = baseOffset + stride * idx;
params.mStartPt = paddingOffset + colIdx;
```

### first_window_overflow-RULE-04：首窗口位置与 fmap 尺寸比较（运行时检测）

**【条件】**LoadData3D 调用前缺少 `kStartPt < fmapH && mStartPt < fmapW` 断言
**【API】**LoadData3D（调用前窗口位置边界校验）
**【约束】**LoadData3D 调用前存在 `ASSERT(kStartPt < fmapSize)` 断言
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

### first_window_overflow-RULE-05：窗口位置加 filter 越界（运行时检测）

**【条件】**缺少 `kStartPt + filterH <= fmapH` 验证，首窗口结束位置可能超出 fmap
**【API】**LoadData3DParams.kStartPt + filterH（首窗口结束位置）
**【约束】**`ASSERT(kStartPt + filterH <= fmapH) ∧ ASSERT(mStartPt + filterW <= fmapW)`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

## 修复方案

### 标准修复模式

1. **修复模式 1（窗口位置 clamp 到 fmap 范围）**：`params.kStartPt = std::min(offsetK, params.fmapInfo.h - params.filterH); ASSERT(params.kStartPt + params.filterH <= params.fmapInfo.h);`
2. **修复模式 2（tiling 驱动的安全窗口计算）**：`uint32_t rawK = blockIdx.y * tileH; params.kStartPt = std::min(rawK, fmapH > filterH ? fmapH - filterH : 0u);`
3. **修复模式 3（多维度联合校验）**：`ASSERT(kPos + filterH <= fmapH); params.kStartPt = std::min(kPos, fmapH - filterH);`
4. **修复模式 4（跳过越界 block）**：`if (kPos + filterH > fmapH) return; params.kStartPt = kPos;`

### 代码模板

模板 1：安全窗口位置计算器（detector 原文）

```cpp
struct WindowPosition {
    uint32_t kStart;
    uint32_t mStart;
};

inline WindowPosition SafeWindowPos(uint32_t kOff, uint32_t mOff, uint32_t fH, uint32_t fW, uint32_t mapH, uint32_t mapW) {
    uint32_t kMax = mapH > fH ? mapH - fH : 0;
    uint32_t mMax = mapW > fW ? mapW - fW : 0;
    return {std::min(kOff, kMax), std::min(mOff, mMax)};
}
```

模板 2：tiling 驱动的安全窗口遍历（detector 原文）

```cpp
inline bool IsValidWindow(uint32_t blockIdx, uint32_t tileSize, uint32_t fmapSize, uint32_t filterSize) {
    return blockIdx * tileSize + filterSize <= fmapSize;
}
```

### 防御性编程建议

1. **始终校验窗口位置在 fmap 内**：在设置 `kStartPt` 和 `mStartPt` 后，验证其加上 filter 尺寸不超过 fmap 边界。
2. **使用 clamp 而非截断**：对窗口位置使用 `min(value, fmapSize - filterSize)` 而非简单截断。
3. **越界 block 跳过**：当 tiling 计算的窗口位置超出 fmap 时，直接跳过该 block 而非尝试加载。
4. **编译期校验常量**：对编译期已知的窗口位置和 fmap 尺寸使用 `static_assert` 校验。
5. **运行时日志**：在关键路径记录窗口位置值，便于首窗口超限问题的快速定位。
6. **边界条件测试**：设计 fmap 尺寸恰好等于 filter 尺寸的边界用例，确保 kStartPt=0 的场景正确处理。

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| kStartPt 过大 | cann-aicore/09_loaddata3d/ld3d_window_overrun_verified | `kStartPt = 1024, fmapH = 256`，k 维度越界 | EC2 |
| mStartPt 过大 | 变体（待生成） | `mStartPt = 2048, fmapW = 256`，m 维度越界 | EC2 |
| 两者同时过大 | 变体（待生成） | `kStartPt = 1024, mStartPt = 2048`，双维度同时越界 | EC2 |
| tiling 驱动的窗口位置计算越界 | 变体（待生成） | `blockIdx × tileLen > fmapSize`，tiling 计算溢出 | EC3 |

## 等价类划分

变异维度：LoadData3DParams.kStartPt/mStartPt 相对于 fmap 尺寸（含 filter 偏移）

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | kStartPt < fmapH ∧ mStartPt < fmapW ∧ kStartPt + filterH ≤ fmapH | 合法范围 | 窗口位置在 fmap 内，安全 |
| EC2 | kStartPt 或 mStartPt 直接赋值过大 | ≥ fmapSize | 编译期可确定，覆盖原始种子 |
| EC3 | tiling 驱动的窗口位置计算越界 | blockIdx × tileSize > fmapSize | 需运行时检查 |

划分依据：硬件对 LoadData3D 首窗口位置的范围检查。EC2 为编译期可确定的常量越界，EC3 需 host 侧 tiling 计算路径分析。

## 关联异常

- **ub_addr_misalign（传播链下游）**：cross_references "错误传播链"段收录：`first_window_overflow → ub_addr_misalign`（LoadData3D 首窗口超限导致 UB 地址非对齐），来源标注"语义推断"。历史种子（legacy: first_window_overflow） 预期触发 mte_f1wpos_larger_fsize 但实际触发 ADDR_MISALIGN（0x10000），印证此传播链——首窗口起点越界在硬件上表现为地址非对齐（ub_addr_misalign）。
- **fmap_size_illegal / padding_misconfig / l1_3d_size_illegal**：同属 fmap 族 + 09_loaddata3d 类别（cross_references 反查 keyword_index：mte_f1wpos_larger_fsize → [first_window_overflow]）。
- **c0_idx_overflow**：同属 LoadData3D 族（cross_references 反查 api_index：LoadData3D → [c0_idx_overflow..l1_3d_size_illegal]）。
- **（推断，非 cross_references 收录）**与 fmap_size_illegal 共享 fmap 尺寸语义（first_window_overflow = 首窗口位置超 fmap，fmap_size_illegal = fmap H/W 为 0），但触发机制不同；与 ub_addr_misalign 通过传播链关联（first_window_overflow → ub_addr_misalign）。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/05）、变量名匹配（RULE-02）、存在性检查（RULE-03/04）
- **命中判定**：
  - 值域判断类规则——提取 `params.(kStartPt|mStartPt) = VALUE` 表达式，VALUE 为字面量大值且 ≥ fmapSize 即命中 RULE-01；进一步验证 `kStartPt + filterH ≤ fmapH ∧ mStartPt + filterW ≤ fmapW`，失败即命中 RULE-05；跨行追溯 ≤3 层
  - 变量名匹配类规则——在源码定位 `params.(kStartPt|mStartPt) = blockIdx.x × tileLen` 或 `blockIdx.x × tileW + threadIdx.x` 表达式，追溯 host 侧 tiling 计算；追溯受限时标记 `trace_limited=true`
  - 存在性检查类规则——窗口位置赋值后是否存在 `min(value, fmapSize - filterSize)` clamp 或 `ASSERT(kStartPt < fmapSize)` 边界校验（缺失即命中 RULE-03）；LoadData3D 调用前是否存在 `kStartPt < fmapH && mStartPt < fmapW` 断言（缺失即命中 RULE-04）
- **修复映射**：
  - first_window_overflow-RULE-01（常量赋值越界）→ 修复模式 1（窗口位置 clamp）
  - first_window_overflow-RULE-02（tiling 计算溢出）→ 修复模式 2（tiling 驱动安全窗口）/ 修复模式 4（跳过越界 block）
  - first_window_overflow-RULE-03（缺少边界校验）→ 修复模式 1 / 修复模式 3（多维度联合校验）
  - first_window_overflow-RULE-04（首窗口位置校验缺失）→ 修复模式 1（添加 ASSERT）
  - first_window_overflow-RULE-05（窗口 + filter 越界）→ 修复模式 3（多维度联合校验）
