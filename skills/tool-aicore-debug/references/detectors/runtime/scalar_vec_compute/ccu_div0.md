# ccu_div0 — CCU 标量整数除零

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | ccu_div0 |
| 异常类型 | ccu_div0 |
| 错误码 | 3 |
| 类别 | scalar_vec_compute |
| 源文件 | cann-aicore/12_scalar/scalar_divzero_verified/error/main.asc |
| 报错模式 | EZ9999_stderr |
| 内存区域 | N/A（CCU 标量单元，无特定 buffer） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

当 kernel 代码中执行整数除法或取模运算时，若除数为零，CCU（Cube Compute Unit）标量单元将触发 ccu_div0 异常（ccu_div0，错误码 3），AI Core 立即停止执行并上报异常。

触发条件（detector 原文，逐字保留）：

1. **直接除零**：代码中显式使用常量零作为除数
2. **变量除零**：除数为运行时变量，在特定条件下值为零
3. **取模运算除零**：取模运算（`%`）的右操作数为零
4. **tiling 参数作为除数为 0**：tiling 计算结果作为除数，但在边界情况下为零

形式化规则前提：`ASSERT_ccu_div0_1: ∀ 除法操作 a / b, b ≠ 0`；`ASSERT_ccu_div0_2: ∀ 取模操作 a % b, b ≠ 0`；`ASSERT_ccu_div0_3: ∀ tiling 除法 totalElements / tileLength, tileLength > 0`。

## 检测规则

### ccu_div0-RULE-01：直接除零模式检测

**【条件】**整数除法或取模运算中除数为字面量零（`/ 0` 或 `% 0`）
**【API】**整数除法 `/` / 取模 `%`（字面量 0 除数）
**【约束】**除数 ≠ 0
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
// 匹配：直接除零
int x = a / 0;
int y = b % 0;
int z = 100 / 0;
```

不匹配（detector 原文）：

```cpp
// 不匹配：除数非零
int x = a / 1;
int y = b / 2;
```

### ccu_div0-RULE-02：变量除零风险检测

**【条件】**除法或取模运算的除数为变量，且该变量可能为零，未在除法前进行非零检查
**【API】**整数除法 `/` / 取模 `%`（变量除数）
**【约束】**除数为变量时存在前置非零检查或使用 `max(divisor, 1)` 保护
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

匹配示例（detector 原文）：

```cpp
// 匹配：变量除零风险
int result = numerator / divisor;
int remainder = value % step;
int tiles = total / tileSize;
```

不匹配（detector 原文）：

```cpp
// 不匹配：已有非零保护
if (divisor != 0) {
    int result = numerator / divisor;
}
int safeDiv = max(divisor, 1);
int result = numerator / safeDiv;
```

### ccu_div0-RULE-03：tiling 参数除零风险检测

**【条件】**tiling 计算结果（如 tileSize、blockLength 等）用作除数但未做最小值约束
**【API】**整数除法 `/`（tiling 参数作为除数）
**【约束】**`tileLength > 0 ∧ blockDim > 0`，tiling 参数有最小值约束
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 变量名匹配

匹配示例（detector 原文）：

```cpp
// 匹配：tiling 参数作为除数
int numTiles = totalElements / tileLength;
int blocks = dataSize / blockDim;
```

不匹配（detector 原文）：

```cpp
// 不匹配：tiling 参数有最小值约束
int safeTileLength = max(tileLength, 1);
int numTiles = totalElements / safeTileLength;
```

### ccu_div0-RULE-04：运行时除数值验证

**【条件】**执行除法前未检查除数是否为零
**【API】**整数除法 `/` / 取模 `%`（运行时除数校验）
**【约束】**除法前存在 `ASSERT(divisor != 0)` / `ASSERT(step > 0)` / `ASSERT(tileSize > 0)` 断言
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

## 修复方案

### 标准修复模式

1. **修复模式 1（除法前添加非零检查）**：`if (divisor != 0) { result = numerator / divisor; }`
2. **修复模式 2（使用安全的除法辅助函数）**：`__aicore__ inline int SafeDiv(int numerator, int divisor, int defaultValue = 0) { return (divisor != 0) ? (numerator / divisor) : defaultValue; }`
3. **修复模式 3（tiling 参数最小值约束）**：`tileLength = max(tileLength, 1); int numTiles = totalElements / tileLength;`
4. **修复模式 4（取模运算安全保护）**：`if (step != 0) { remainder = value % step; }`

### 代码模板

模板 1：通用安全数学运算工具（detector 原文）

```cpp
__aicore__ inline int SafeDiv(int a, int b, int defaultVal = 0) {
    return (b != 0) ? (a / b) : defaultVal;
}

__aicore__ inline int SafeMod(int a, int b, int defaultVal = 0) {
    return (b != 0) ? (a % b) : defaultVal;
}

__aicore__ inline int SafeCeilDiv(int a, int b) {
    return (b != 0) ? ((a + b - 1) / b) : 0;
}

__aicore__ inline int AlignUp(int value, int alignment) {
    if (alignment == 0) return value;
    return ((value + alignment - 1) / alignment) * alignment;
}
```

### 防御性编程建议

1. **所有除法操作默认使用安全封装**：使用 `SafeDiv` / `SafeMod` 替代直接 `/` 和 `%` 运算符
2. **tiling 参数设置最小值**：所有作为除数的 tiling 参数应保证 `min(value, 1)`
3. **断言除数非零**：在关键除法前添加 `ASSERT(divisor != 0)` 作为调试期保护
4. **代码审查关注除法**：提交前检查所有 `/` 和 `%` 操作是否已做零值保护
5. **区分浮点除法和整数除法**：浮点除法不触发 ccu_div0，但结果可能为 inf/nan，仍需保护
6. **提供合理的默认值**：当除数为零时返回有意义的默认值（如 0 或 1），避免后续计算异常

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| 直接除零 | cann-aicore/12_scalar/scalar_divzero_verified | `int x = 10 / 0;` 常量零作为除数 | EC2 |
| 变量除零 | 变体（待生成） | `result = a / var;`（var=0）运行时变量为零 | EC3 |
| 取模运算除零 | 变体（待生成） | `int m = 10 % 0;` 取模右操作数为零 | EC2 |
| tiling 参数作为除数为 0 | 变体（待生成） | `tiles = total / tileSize;`（tileSize=0） | EC3 |

## 等价类划分

变异维度：整数除法/取模的除数取值

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | 除数 > 0 | 正整数 | 安全 |
| EC2 | 除数为字面量 0 | 字面量 0 | 编译期可确定，覆盖原始种子 |
| EC3 | 除数为变量/tiling 参数，边界条件下为 0 | 运行时 = 0 | 需运行时分析 |

划分依据：CCU 标量单元对整数除法/取模的除数合法性检查。EC2 为编译期可确定的字面量除零，EC3 需运行时变量值分析。

## 关联异常

- **GetValue**：共享 API（cross_references 反查 api_index：GetValue → [ccu_div0]，CCU 标量单元从 UB 读取标量值用于除法运算）。
- **（推断，非 cross_references 收录）**与 mmad_illegal/fixpipe_cfg_illegal 共享"tiling 除法产生 0"语义模式（ccu_div0 = 除数为 0 触发 ccu_div0，mmad_illegal = Mmad 维度为 0 触发 cube_illegal_instr，fixpipe_cfg_illegal = FixPipe 参数为 0 触发 fixp_err_illegal_cfg），但异常类型与 API 不同。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01）、存在性检查（RULE-02/04）、变量名匹配（RULE-03）
- **命中判定**：
  - 值域判断类规则——全文模式搜索 `/ 0` 与 `% 0` 字面量除零模式，命中即报告；跨行追溯 ≤3 层
  - 存在性检查类规则——提取 `a / var` / `a % var` 表达式，反查 var 是否有前置 `if (var != 0)` 非零检查或 `max(var, 1)` 保护（缺失即命中 RULE-02）；除法前是否存在 `ASSERT(divisor != 0)` 断言（缺失即命中 RULE-04）
  - 变量名匹配类规则——在源码定位 `total / tiling.FIELD` 表达式，追溯 tiling 字段赋值与最小值约束；追溯受限时标记 `trace_limited=true`
- **修复映射**：
  - ccu_div0-RULE-01（直接除零）→ 修复模式 1（添加非零检查）/ 修复模式 2（SafeDiv 封装）
  - ccu_div0-RULE-02（变量除零风险）→ 修复模式 1 / 修复模式 2
  - ccu_div0-RULE-03（tiling 除零风险）→ 修复模式 3（tiling 最小值约束）
  - ccu_div0-RULE-04（运行时除数验证）→ 修复模式 1（ASSERT 保护）
