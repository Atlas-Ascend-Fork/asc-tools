# fixpipe_cfg_illegal — FixPipe 配置非法

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | fixpipe_cfg_illegal |
| 异常类型 | fixp_err_illegal_cfg |
| 错误码 | 101 |
| 类别 | fixpipe |
| 源文件 | cann-aicore/10_fixpipe/fixpipe_illegal_cfg_verified/error/main.asc |
| 报错模式 | EZ9999_stderr |
| 内存区域 | CUBE_OUT（FixPipe 配置非法，Cube 输出阶段） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

在 FixPipe 操作中，`ndNum`、`mSize` 或 `nSize` 参数被设置为 0。硬件无法根据非法配置执行 FixPipe 操作，触发 fixp_err_illegal_cfg 异常（错误码 101），操作直接失败。同属 cross_references "症状相似"段：FixPipe overflow = {fixpipe_gm_overflow, fixpipe_cfg_illegal, fixpipe_l1_overflow, atomic_fixpipe_overflow}（区分要点：fixpipe_cfg_illegal = 配置参数非法触发 fixp_err_illegal_cfg）。

触发条件（detector 原文，逐字保留）：

- C1：`ndNum == 0`（循环次数为 0）
- C2：`mSize == 0`（M 维度大小为 0）
- C3：`nSize == 0`（N 维度大小为 0）
- C4：多参数同时为 0（多个维度同时非法）

形式化规则前提：`ASSERT_fixpipe_cfg_illegal_1: params.ndNum >= 1`、`ASSERT_fixpipe_cfg_illegal_2: params.mSize >= 1`、`ASSERT_fixpipe_cfg_illegal_3: params.nSize >= 1`。

## 检测规则

### fixpipe_cfg_illegal-RULE-01：关键参数直接赋值为 0

**【条件】**代码中出现 `(ndNum|mSize|nSize) = 0` 模式，关键参数直接设置为 0
**【API】**FixPipeParams.ndNum / mSize / nSize 赋值
**【约束】**`ndNum >= 1 ∧ mSize >= 1 ∧ nSize >= 1`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
fp.ndNum = 0;
fp.mSize = 0;
fp.nSize = 0;
```

### fixpipe_cfg_illegal-RULE-02：tiling 除法可能产生 0

**【条件】**`(ndNum|mSize|nSize) = EXPR / (tileSize|blockDim|groupNum)`，参数由除法计算且除数大于被除数时结果为 0
**【API】**FixPipeParams.ndNum / mSize / nSize 赋值（除法表达式）
**【约束】**除法结果 ≥ 1（除数 ≤ 被除数）
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
fp.ndNum = totalRows / tileH;
fp.mSize = channels / groupSize;
fp.nSize = width / blockW;
```

### fixpipe_cfg_illegal-RULE-03：参数初始化不完整

**【条件】**FixPipeParams 创建后未设置所有必要参数（ndNum/mSize/nSize 中至少一个未赋值）
**【API】**FixPipeParams 构造与字段赋值
**【约束】**`∀ FixPipeParams 实例: all_required_fields_set == true`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

匹配示例（detector 原文）：

```cpp
FixPipeParams<float> fp;
fp.mSize = 128;
fp.nSize = 256;
// ndNum 未设置
```

### fixpipe_cfg_illegal-RULE-04：参数非零校验缺失（运行时检测）

**【条件】**FixPipe 调用前缺少 `ndNum > 0 && mSize > 0 && nSize > 0` 断言
**【API】**FixPipe（调用前非零校验）
**【约束】**FixPipe 调用前存在 `ASSERT(fp.ndNum > 0 && fp.mSize > 0 && fp.nSize > 0)`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

### fixpipe_cfg_illegal-RULE-05：tiling 参数传递校验（运行时检测）

**【条件】**tiling 结构体中 ndNum/mSize/nSize 字段无默认值保护
**【API】**tiling 结构体字段（ndNum/mSize/nSize）
**【约束】**`max(tiling.ndNum, 1) >= 1 ∧ max(tiling.mSize, 1) >= 1`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

## 修复方案

### 标准修复模式

1. **修复模式 1（参数最小值保护）**：`fp.ndNum = std::max(tiling.ndNum, 1u); fp.mSize = std::max(tiling.mSize, 1u); fp.nSize = std::max(tiling.nSize, 1u); ASSERT(...);`
2. **修复模式 2（tiling 除法结果保护）**：`fp.mSize = std::max(totalM / std::max(groupSize, 1u), 1u);`
3. **修复模式 3（完整参数初始化）**：FixPipeParams 创建后立即设置所有必要参数（含 ndNum 默认值）
4. **修复模式 4（条件执行保护）**：`if (fp.ndNum == 0 || fp.mSize == 0 || fp.nSize == 0) return;`

### 代码模板

模板 1：安全 FixPipe 参数构建器（detector 原文）

```cpp
template<typename T>
inline FixPipeParams<T> BuildSafeFixPipeParams(uint32_t ndNum, uint32_t m, uint32_t n) {
    FixPipeParams<T> fp;
    fp.ndNum = std::max(ndNum, 1u);
    fp.mSize = std::max(m, 1u);
    fp.nSize = std::max(n, 1u);
    ASSERT(fp.ndNum > 0 && fp.mSize > 0 && fp.nSize > 0);
    return fp;
}
```

模板 2：FixPipe 参数校验宏（detector 原文）

```cpp
#define VALIDATE_FIXPIPE_PARAMS(fp) \
    do { \
        ASSERT((fp).ndNum > 0); \
        ASSERT((fp).mSize > 0); \
        ASSERT((fp).nSize > 0); \
    } while(0)
```

### 防御性编程建议

1. **参数初始化完整**：FixPipeParams 创建后立即设置所有必要参数，不依赖默认值。
2. **tiling 计算保护**：对所有 tiling 驱动的参数计算使用 `max(result, 1)` 保护。
3. **调用前断言校验**：在每次 FixPipe 调用前添加 `ndNum > 0 && mSize > 0 && nSize > 0` 断言。
4. **除数非零保护**：在 tiling 除法计算中确保除数不为 0，使用 `max(divisor, 1)` 保护。
5. **条件执行**：当参数可能为 0 时，添加条件判断跳过 FixPipe 调用。
6. **单元测试覆盖零值**：对每个参数单独测试为 0 的场景，确保修复有效。

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| ndNum=0 | cann-aicore/10_fixpipe/fixpipe_illegal_cfg_verified | `fp.ndNum = 0`，循环次数非法 | EC2 |
| mSize=0 | 变体（待生成） | `fp.mSize = 0`，M 维度为 0 | EC2 |
| nSize=0 | 变体（待生成） | `fp.nSize = 0`，N 维度为 0 | EC2 |
| 多参数同时为 0 | 变体（待生成） | `ndNum=0, mSize=0, nSize=0`，配置完全非法 | EC2 |
| tiling 驱动的参数为 0 | 变体（待生成） | `tiling.mSize = 0`，tiling 计算结果为 0 | EC3 |

## 等价类划分

变异维度：FixPipeParams.ndNum/mSize/nSize 是否 ≥ 1

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | ndNum ≥ 1 ∧ mSize ≥ 1 ∧ nSize ≥ 1 | 合法范围 | 配置合法，安全 |
| EC2 | 任一参数直接赋值为 0 | = 0 | 编译期常量可确定，覆盖原始种子 |
| EC3 | tiling 除法结果为 0 | divisor > dividend | 需 host 侧 tiling 计算路径分析 |

划分依据：FixPipe 硬件对配置参数的合法性检查。EC2 为编译期可确定，EC3 需 tiling 计算路径分析（运行时检查）。

## 关联异常

- **fixpipe_gm_overflow / fixpipe_l1_overflow / atomic_fixpipe_overflow**：同属 cross_references "症状相似"段：FixPipe overflow = {fixpipe_gm_overflow, fixpipe_cfg_illegal, fixpipe_l1_overflow, atomic_fixpipe_overflow}。区分要点（cross_references 原文）：fixpipe_gm_overflow = FixPipe 写 GM 溢出（错误码 141）；fixpipe_cfg_illegal = FixPipe 配置非法（错误码 101，fixp_err_illegal_cfg）；fixpipe_l1_overflow = FixPipe 读 L1 越界（错误码 103）；atomic_fixpipe_overflow = FixPipe AtomicAdd GM 溢出（错误码 141，与 fixpipe_gm_overflow 碰撞）。
- **FixPipe**：共享 API（cross_references 反查 api_index：FixPipe → [fixpipe_gm_overflow, fixpipe_cfg_illegal, fixpipe_l1_overflow, atomic_fixpipe_overflow, fixpipe_srcstride_overflow]）。
- **（推断，非 cross_references 收录）**与 mmad_illegal 共享"参数为 0 触发非法配置"语义模式（fixpipe_cfg_illegal = FixPipe ndNum/mSize/nSize 为 0，mmad_illegal = Mmad m/n/k 为 0），但 API 不同。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02）、存在性检查（RULE-03/04/05）
- **命中判定**：
  - 值域判断类规则——提取 `fp.(ndNum|mSize|nSize) = EXPR` 表达式，EXPR 为字面量 0 即命中 RULE-01；EXPR 为除法表达式时追溯除数与被除数关系，除数 > 被除数即命中 RULE-02；跨行追溯 ≤3 层
  - 存在性检查类规则——全文模式搜索 FixPipeParams 实例化后是否设置所有必要字段（缺失即命中 RULE-03）；FixPipe 调用前是否存在 `ndNum > 0 && mSize > 0 && nSize > 0` 断言（缺失即命中 RULE-04）；tiling 结构体字段是否有 max 保护（缺失即命中 RULE-05）
- **修复映射**：
  - fixpipe_cfg_illegal-RULE-01（参数直接为 0）→ 修复模式 1（最小值保护）
  - fixpipe_cfg_illegal-RULE-02（tiling 除法产生 0）→ 修复模式 2（除法结果保护）
  - fixpipe_cfg_illegal-RULE-03（参数初始化不完整）→ 修复模式 3（完整初始化）
  - fixpipe_cfg_illegal-RULE-04（非零校验缺失）→ 修复模式 1（添加 ASSERT）/ 修复模式 4（条件执行保护）
  - fixpipe_cfg_illegal-RULE-05（tiling 参数传递）→ 修复模式 2
