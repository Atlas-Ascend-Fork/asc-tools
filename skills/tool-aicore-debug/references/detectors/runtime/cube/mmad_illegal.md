# mmad_illegal — MMAD 参数非法

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | mmad_illegal |
| 异常类型 | cube_illegal_instr |
| 错误码 | 126 |
| 类别 | cube |
| 源文件 | cann-aicore/07_cube_compute/mmad_zero_param_verified/error/main.asc |
| 报错模式 | EZ9999_stderr |
| 内存区域 | CUBE_OUT（Cube 计算单元，Mmad 输出 L0C） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

Mmad（矩阵乘加）指令的参数 m、n、k 中至少一个为 0 时，Cube 计算单元无法执行矩阵运算，硬件触发非法指令异常 cube_illegal_instr（错误码 126）。Mmad 的 m、n、k 参数定义了矩阵乘法的维度，必须为合法正整数（如 m=16、32 等），为 0 时指令语义无效。

触发条件（detector 原文，逐字保留）：

- Mmad 参数 m 被设置为 0
- Mmad 参数 n 被设置为 0
- Mmad 参数 k 被设置为 0
- 多个 m/n/k 参数同时为 0
- tiling 参数驱动的维度计算结果为 0

形式化规则前提：`ASSERT mmad_illegal-PARAM-01: ∀ Mmad 调用: PARAMS.m > 0 ∧ PARAMS.n > 0 ∧ PARAMS.k > 0`。

## 检测规则

### mmad_illegal-RULE-01：Mmad 参数直接赋值为 0

**【条件】**代码中出现 `mmadParams.(m|n|k) = 0`，Mmad 的 m/n/k 字段被赋值为 0
**【API】**MmadParams.m / n / k 赋值（字面量 0）
**【约束】**`mmadParams.m > 0 ∧ mmadParams.n > 0 ∧ mmadParams.k > 0`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
mmadParams.m = 0;
mmadParams.n = 16;
mmadParams.k = 16;
Mmad(c1, a2, b2d, mmadParams);
```

### mmad_illegal-RULE-02：Tiling 参数驱动的 Mmad 参数为 0

**【条件】**`mmadParams.(m|n|k) = tiling.dimM/dimN/dimK`，tiling 参数中用于 Mmad 维度的字段可能为 0
**【API】**MmadParams.m / n / k 赋值（tiling 驱动）
**【约束】**`tiling.dimM > 0 ∧ tiling.dimN > 0 ∧ tiling.dimK > 0`，host 侧 tiling 计算路径中 dimM/dimN/dimK 边界条件下不为 0
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 变量名匹配

匹配示例（detector 原文）：

```cpp
mmadParams.m = tiling.dimM;
mmadParams.n = tiling.dimN;
mmadParams.k = tiling.dimK;
```

### mmad_illegal-RULE-03：算术表达式驱动的维度为 0

**【条件】**`mmadParams.(m|n|k) = totalM - usedM` 或 `mmadParams.k = remainK / blockK`，减法/除法表达式可能产生 0 或负值
**【API】**MmadParams.m / n / k 赋值（算术表达式）
**【约束】**减法表达式 `total > used`；除法表达式 `remain >= block`（确保结果 ≥ 1）
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
mmadParams.m = totalM - usedM;
mmadParams.k = remainK / blockK;
```

### mmad_illegal-RULE-04：Mmad 调用前参数校验（运行时检测）

**【条件】**Mmad 调用前未检查 `mmadParams.m > 0 && mmadParams.n > 0 && mmadParams.k > 0`
**【API】**Mmad（调用前参数校验）
**【约束】**Mmad 调用前存在 `ASSERT(m > 0 && n > 0 && k > 0)` 断言
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

### mmad_illegal-RULE-05：边界条件下的 tiling 计算（运行时检测）

**【条件】**tiling 计算中 `totalSize % tileSize == 0` 时最后一个 tile 的维度为 0，或 `totalSize < tileSize` 时维度计算结果为 0
**【API】**tiling 计算（边界条件）
**【约束】**循环尾部残余块维度 ≥ 1；输入 size < tileSize 时维度计算 ≥ 1
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

## 修复方案

### 标准修复模式

1. **修复模式 1（修正 Mmad 参数为合法值）**：`mmadParams.m = 16; mmadParams.n = 16; mmadParams.k = 16;`（直接赋值为合法正整数）
2. **修复模式 2（Tiling 参数下界保护）**：`mmadParams.m = (tiling.dimM > 0) ? tiling.dimM : 16;`
3. **修复模式 3（算术表达式结果保护）**：`uint32_t computedM = totalM - usedM; mmadParams.m = (computedM > 0) ? computedM : 16;`
4. **修复模式 4（Host 侧 tiling 计算修正）**：`tiling.dimM = (totalSize / tileM > 0) ? (totalSize / tileM) : 1;`

### 代码模板

模板：安全的 Mmad 参数设置（detector 原文）

```cpp
__aicore__ inline void SafeMmadParams(
    MmadParams& params,
    uint32_t m, uint32_t n, uint32_t k)
{
    params.m = (m > 0) ? m : 16;
    params.n = (n > 0) ? n : 16;
    params.k = (k > 0) ? k : 16;
}
```

模板：带边界检查的 Mmad 调用（detector 原文）

```cpp
template<typename T>
__aicore__ inline void SafeMmad(
    LocalTensor<T>& dst,
    LocalTensor<T>& srcA,
    LocalTensor<T>& srcB,
    MmadParams& params)
{
    if (params.m > 0 && params.n > 0 && params.k > 0) {
        Mmad(dst, srcA, srcB, params);
    }
}
```

### 防御性编程建议

1. **Mmad 参数合法性断言**：在 Mmad 调用前添加 `ASSERT(m > 0 && n > 0 && k > 0)`
2. **Tiling 计算下界保护**：host 侧所有用于 Mmad 维度的参数计算结果应确保 ≥ 1
3. **边界条件测试**：针对输入 size 等于 tile size 整数倍和非整数倍两种情况分别测试
4. **参数校验函数**：封装 Mmad 参数校验工具函数，统一检查 m/n/k 合法性
5. **循环尾部残余块处理**：在 tiling 循环的最后一次迭代中特别检查 Mmad 参数是否合法
6. **Host 侧参数传递审查**：检查 host 到 device 的参数传递链路，确保无 0 值泄漏

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| m=0 | cann-aicore/07_cube_compute/mmad_zero_param_verified | `mmadParams.m = 0`，矩阵乘法的行维度为 0 | EC2 |
| n=0 | 变体（待生成） | `mmadParams.n = 0`，矩阵乘法的列维度为 0 | EC2 |
| k=0 | 变体（待生成） | `mmadParams.k = 0`，矩阵乘法的内积维度为 0 | EC2 |
| 多个参数同时为 0 | 变体（待生成） | `mmadParams.m/n/k` 多个为 0 | EC2 |
| tiling 驱动的参数计算结果为 0 | 变体（待生成） | tiling 参数计算路径边界条件下产生 0 值 | EC3 |

## 等价类划分

变异维度：MmadParams.m/n/k 是否 > 0

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | m > 0 ∧ n > 0 ∧ k > 0 | 合法范围（如 m=16, n=16, k=16） | 维度合法，安全 |
| EC2 | 任一参数直接赋值为 0 | = 0 | 编译期可确定，覆盖原始种子 |
| EC3 | tiling 驱动的参数计算结果为 0 | 边界条件下 tiling 计算产生 0 | 需运行时分析 |

划分依据：Cube 硬件对 Mmad 指令参数的合法性检查。EC2 编译期可确定，EC3 需 host 侧 tiling 计算路径分析（运行时检查）。

## 关联异常

- **cube_nan_inf**：同属 07_cube_compute 类别（cross_references 反查 keyword_index：cube_invld_input → [cube_nan_inf]，cube_illegal_instr → [mmad_illegal]；MMAD/Mmad → [mmad_illegal]）。mmad_illegal 为参数非法触发 cube_illegal_instr，cube_nan_inf 为输入 NaN/Inf 触发 cube_invld_input，两者均为 Cube 计算异常但触发机制不同。
- **（推断，非 cross_references 收录）**与 fixpipe_cfg_illegal 共享"参数为 0 触发非法配置"语义模式（mmad_illegal = Mmad m/n/k 为 0，fixpipe_cfg_illegal = FixPipe ndNum/mSize/nSize 为 0），但 API 不同；与 ccu_div0 共享 tiling 除法产生 0 的语义模式（推断）。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/03/05）、变量名匹配（RULE-02）、存在性检查（RULE-04）
- **命中判定**：
  - 值域判断类规则——提取 `mmadParams.(m|n|k) = EXPR` 表达式，EXPR 为字面量 0 即命中 RULE-01；EXPR 为减法/除法表达式时追溯操作数关系（减法 total < used、除法 remain < block 即命中 RULE-03）；跨行追溯 ≤3 层
  - 变量名匹配类规则——在源码定位 `mmadParams.(m|n|k) = tiling.dim(M|N|K)` 表达式，追溯 host 侧 tiling 计算路径；追溯受限时标记 `trace_limited=true`
  - 存在性检查类规则——全文模式搜索 Mmad 调用前是否存在 `m > 0 && n > 0 && k > 0` 断言（缺失即命中 RULE-04）
- **修复映射**：
  - mmad_illegal-RULE-01（参数直接为 0）→ 修复模式 1（修正为合法值）
  - mmad_illegal-RULE-02（tiling 驱动为 0）→ 修复模式 2（tiling 下界保护）/ 修复模式 4（host 侧 tiling 修正）
  - mmad_illegal-RULE-03（算术表达式为 0）→ 修复模式 3（表达式结果保护）
  - mmad_illegal-RULE-04（调用前校验缺失）→ 修复模式 1 / 模板：带边界检查的 Mmad 调用
  - mmad_illegal-RULE-05（边界条件 tiling）→ 修复模式 4（host 侧 tiling 修正）
