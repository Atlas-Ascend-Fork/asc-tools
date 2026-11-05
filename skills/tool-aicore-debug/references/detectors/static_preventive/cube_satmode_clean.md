# cube_satmode_clean — CUBE 输入异常值检查与按需清洗

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | cube_satmode_clean |
| 异常类型 | 静态预防（输入 NaN/Inf 风险） |
| 错误码 | 无（本条目不依赖是否产生硬件上报） |
| 类别 | static_preventive |
| 性质 | 预防型 |
| 覆盖条目 | A-2.3 L16（CUBE NaN/Inf 计算风险） |
| 检测时机 | 输入准备阶段、GM→L1 搬运前 |
| 报错模式 | 随目标架构、API、软件栈和输入而异；不保证硬件静默 |

## 触发条件

CUBE 输入可能含 NaN/Inf，而目标算子要求有限输入时，检查数据来源和上游计算。硬件是否上报以及是否存在 saturation 支持须按当前目标确认；不能把本静态条目的“不依赖报错”解释为所有环境都不会报错，也不能与 cube_nan_inf 的历史码 9 互相覆盖。

若算子本来要求传播 NaN/Inf，不能为了消除告警将其清零。缺少 IsNan/IsInf 调用不代表输入实际含异常值。

## 检测规则

### cube_satmode_clean-RULE-01：违反已确认的有限输入要求

【条件】算子要求有限输入，且输入准备或上游操作已确认生成 NaN/Inf 并流入 Mmad
【API】Host 输入准备 / Mmad / LoadData 数据流
【约束】仅在有限输入属于目标语义时校验；缺少清洗调用本身只作为防御建议，不证明异常输入
【匹配模式】值域判断

## 修复方案

### 标准修复模式

1. 定位异常输入的来源，优先修正上游除零、溢出或错误初始化。
2. 目标语义允许时，可提出显式替换 NaN/Inf 的方案；替换策略必须来自目标语义，不能自行决定 NaN→0、Inf→最大值。
3. 下方 Host 辅助函数只检查输入，不改变数据。Device-only 输入需采用目标支持的检查方式，不能直接对 GM 指针调用此函数。

### 代码模板

```cpp
#include <cmath>
#include <cstddef>

inline bool HasOnlyFiniteHostInput(const float* values, std::size_t count) {
    if (values == nullptr && count != 0) return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) return false;
    }
    return true;
}
```

### 防御性编程建议

- 明确输入是否允许 NaN/Inf，再选择校验或替换行为。
- 不从字段名、缺失清洗调用或 saturation 设置单独推断硬件异常。

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| 上游产生 NaN/Inf | A-2.3 L16 数据流方向 | 有限输入要求被违反 | EC2 |
| 极端有限输入溢出 | 研究方向 | 需核对实际计算和运行值 | EC3 |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1 | 输入满足当前算子要求 | 有限输入已验证，或目标语义允许 NaN/Inf |
| EC2 | 输入要求违反 | 有限输入要求与实际 NaN/Inf 证据俱全 |
| EC3 | 待验证 | 数据源或目标语义未知 |

## 关联异常

- cube_nan_inf：历史运行时输入异常候选，日志/环境须单独确认。
- vec_nan_inf / scalar_fp_nan_inf：上游异常值传播方向；不能用 OOB 兜底证明浮点异常。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01），缺少检查仅作为防御建议。
- **命中判定**：核对有限输入要求及实际异常值证据。数据源跨函数无法定值时 `trace_limited=true`；运行输入或语义未知时 `runtime_required=true`。
- **修复映射**：优先修正上游数据源；语义允许的显式清洗可作为候选，否则只增加检查并报告，不自动改变输入。
