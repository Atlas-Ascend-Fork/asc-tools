# cube_nan_inf — Cube 输入 NaN/Inf

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | cube_nan_inf |
| 异常类型 | cube_invld_input |
| 错误码 | 9 |
| 类别 | cube |
| 源文件 | cann-aicore/07_cube_compute/cube_nan_input_verified/error/main.asc |
| 报错模式 | EZ9999_stderr |
| 内存区域 | GM（NaN/Inf 输入数据由 host 侧填充至 GM） |
| kernel_diff_type | host_only_diff |
| behavior_note | kernel 函数体与修复版完全一致，差异仅在 host 侧（NaN 输入由 host 侧填充 0xFF 触发） |

## 触发条件

Cube 计算单元（Mmad）的输入数据中包含 NaN（非数字）或 Inf（无穷大）等异常浮点值时，硬件检测到非法输入并触发 cube_invld_input 异常（错误码 9）。这些异常值通常来自 host 侧数据初始化时的不当赋值，或前置计算过程中产生的溢出、除零等结果。本异常为 host_only_diff：原始版/修复版 kernel 类逻辑完全一致（仅类名/构造函数名不同），NaN 输入由 host 侧 `aclrtMemset` 将 GM 数据填充为 0xFF（全 NaN half）触发。

触发条件（detector 原文，逐字保留）：

- Host 侧使用 `std::numeric_limits<float>::quiet_NaN()` 填充 GM 数据
- Host 侧使用 `std::numeric_limits<float>::infinity()` 填充 GM 数据
- 前置计算产生 NaN（如 0/0、Inf-Inf）后作为 Cube 输入
- 前置计算产生 Inf（如 1.0/0.0）后作为 Cube 输入

形式化规则前提：`ASSERT cube_nan_inf-INPUT-01: ∀ GM buffer G 作为 Cube 输入, ∀ 赋值 V 到 G[i]: V != NaN ∧ V != +Inf ∧ V != -Inf ∧ isfinite(V) == true`。

## 检测规则

### cube_nan_inf-RULE-01：Host 侧显式 NaN/Inf 赋值

**【条件】**host 侧代码中对 GM buffer 的 NaN/Inf 赋值（`std::numeric_limits<float>::quiet_NaN()` / `signaling_NaN()` / `infinity()` / `-infinity()`），或使用 `NAN`、`INFINITY` 宏，或 `0.0 / 0.0`、`1.0 / 0.0` 表达式
**【API】**`std::numeric_limits<float>::quiet_NaN` / `signaling_NaN` / `infinity`（host 侧 GM 赋值）
**【约束】**GM buffer 作为 Cube 输入时所有元素 isfinite(V) == true
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

匹配示例（detector 原文）：

```cpp
aGM[i] = std::numeric_limits<float>::quiet_NaN();
bGM[i] = std::numeric_limits<float>::infinity();
```

### cube_nan_inf-RULE-02：可能产生 NaN/Inf 的计算表达式

**【条件】**代码中出现除法操作且除数可能为 0，或乘法操作可能导致溢出，或 `Inf - Inf`、`0 * Inf` 等产生 NaN 的表达式，结果赋值到 GM buffer
**【API】**整数/浮点除法 `/` / 乘法 `*`（产生 NaN/Inf 的表达式）
**【约束】**除法操作除数 != 0；乘法操作结果不溢出；表达式不使用 numeric_limits NaN/Inf 相关值
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
float result = a / b;
float result = a * std::numeric_limits<float>::infinity();
float result = 0.0f / 0.0f;
float result = std::numeric_limits<float>::infinity() - std::numeric_limits<float>::infinity();
```

### cube_nan_inf-RULE-03：GM 数据合法性校验缺失（运行时检测）

**【条件】**kernel 入口处未对 GM 数据进行 `isfinite()` 检查，LoadData 加载数据到 Cube 前未验证输入数据有效性
**【API】**`std::isfinite`（GM 数据校验）
**【约束】**kernel 入口处存在对 GM 输入数据的 isfinite 校验
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

### cube_nan_inf-RULE-04：前置计算的异常值传播（运行时检测）

**【条件】**前置 kernel 的输出可能包含 NaN/Inf 并传播到 Cube 输入，多个 kernel 级联时异常值可能传播
**【API】**多 kernel 级联数据流（异常值传播）
**【约束】**前置计算 C 的所有中间结果 isfinite()，不包含未保护的除零操作
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

## 修复方案

### 标准修复模式

1. **修复模式 1（替换 NaN/Inf 初始化为正常值）**：`aGM[i] = 1.0f; bGM[i] = 2.0f;`（替换 NaN/Inf 为正常浮点值）
2. **修复模式 2（添加数据合法性校验）**：`if (!std::isfinite(xGM[i])) xGM[i] = 0.0f;`（运行时校验并替换异常值）
3. **修复模式 3（前置计算溢出保护）**：`if (denominator != 0.0f && std::isfinite(numerator)) { result = numerator / denominator; if (!std::isfinite(result)) result = 0.0f; }`
4. **修复模式 4（负无穷值修复）**：`aGM[i] = -1.0f;`（替换 -infinity 为有限负值）

### 代码模板

模板：安全的 GM 数据初始化（detector 原文）

```cpp
template<typename T>
void SafeInitGM(GM_ADDR addr, uint32_t len, T value) {
    auto gmPtr = (__gm__ T*)addr;
    for (uint32_t i = 0; i < len; i++) {
        gmPtr[i] = value;
    }
}
```

模板：GM 数据合法性校验（detector 原文）

```cpp
template<typename T>
bool ValidateGMInput(GM_ADDR addr, uint32_t len) {
    auto gmPtr = (__gm__ T*)addr;
    for (uint32_t i = 0; i < len; i++) {
        if (!std::isfinite(gmPtr[i])) return false;
    }
    return true;
}
```

### 防御性编程建议

1. **避免测试数据中使用 NaN/Inf**：除非专门测试异常输入，否则 GM 初始化使用正常浮点值
2. **输入数据合法性检查**：在 kernel 入口处对 GM 输入数据进行 `isfinite()` 校验
3. **前置计算保护**：所有可能产生 NaN/Inf 的计算（除法、开方等）添加保护逻辑
4. **级联 kernel 间数据校验**：多 kernel 级联时，每个 kernel 输出前检查数据合法性
5. **Host 侧单元测试**：对 host 侧数据初始化逻辑进行独立测试，确保不产生异常值
6. **调试阶段添加数据打印**：在调试模式下打印 Cube 输入数据的前若干元素，快速定位异常值

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| 输入含 NaN | cann-aicore/07_cube_compute/cube_nan_input_verified | host 侧 `aclrtMemset` 填充 0xFF（全 NaN half） | EC2 |
| 输入含 Inf | 变体（待生成） | GM 数据中包含正无穷 `infinity()` | EC2 |
| 输入含 -Inf | 变体（待生成） | GM 数据中包含负无穷 `-infinity()` | EC2 |
| 计算过程产生 NaN/Inf 后作为 Cube 输入 | 变体（待生成） | 前置 kernel 产生异常值并传递给后续 Cube 计算 | EC3 |

## 等价类划分

变异维度：Cube 输入数据中 NaN/Inf 的来源

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | 所有输入 isfinite(V) | 有限浮点值 | 安全 |
| EC2 | host 侧显式赋值 NaN/Inf | NaN / +Inf / -Inf | 编译期可确定，覆盖原始种子 |
| EC3 | 前置计算产生 NaN/Inf 传播 | 运行时产生异常值 | 需数据流分析 |

划分依据：Cube 硬件对 Mmad 输入数据的合法性检查。EC2 为 host 侧显式 NaN/Inf 初始化（编译期可确定），EC3 需前置计算数据流分析（运行时检查）。

## 关联异常

- **mmad_illegal**：同属 07_cube_compute 类别（cross_references 反查 keyword_index：cube_invld_input → [cube_nan_inf]，cube_illegal_instr → [mmad_illegal]；NaN → [cube_nan_inf]，MMAD/Mmad → [mmad_illegal]）。cube_nan_inf 为输入 NaN/Inf 触发 cube_invld_input，mmad_illegal 为参数非法触发 cube_illegal_instr，两者均为 Cube 计算异常但触发机制不同。
- **（推断，非 cross_references 收录）**与 ccu_div0 共享"host 侧数据/计算产生异常"语义模式（cube_nan_inf = NaN/Inf 输入，ccu_div0 = 整数除零），但触发单元不同（cube_nan_inf = Cube，ccu_div0 = CCU 标量）；本异常为 host_only_diff（kernel 函数体无差异），与其他 host_only_diff 异常（ub_addr_misalign、tiling_ub_misalign）有结构性区别。

## 检测执行指引

- **适用匹配模式**：API 调用模式（RULE-01/04）、值域判断（RULE-02）、存在性检查（RULE-03）
- **命中判定**：
  - API 调用模式类规则——全文模式搜索 `std::numeric_limits<float>::(quiet_NaN|signaling_NaN|infinity)`、`NAN`、`INFINITY` 调用与 GM 赋值的组合；多 kernel 级联时追踪前置 kernel 输出数据流；CFG 跨函数追溯受限时标记 `trace_limited=true`
  - 值域判断类规则——提取 `a / b` / `a * INF` / `0.0 / 0.0` / `INF - INF` 表达式，判定是否可能产生 NaN/Inf；跨行追溯 ≤3 层
  - 存在性检查类规则——kernel 入口处是否存在对 GM 输入数据的 `isfinite()` 校验（缺失即命中 RULE-03）
- **修复映射**：
  - cube_nan_inf-RULE-01（host 侧显式 NaN/Inf）→ 修复模式 1（替换为正常值）
  - cube_nan_inf-RULE-02（计算表达式产生 NaN/Inf）→ 修复模式 3（前置计算溢出保护）
  - cube_nan_inf-RULE-03（GM 数据校验缺失）→ 修复模式 2（添加 isfinite 校验）
  - cube_nan_inf-RULE-04（前置计算传播）→ 修复模式 3 / 修复模式 2（级联 kernel 间校验）
