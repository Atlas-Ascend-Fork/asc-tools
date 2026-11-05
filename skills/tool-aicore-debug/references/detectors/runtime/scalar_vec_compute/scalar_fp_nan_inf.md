# scalar_fp_nan_inf — 标量浮点输入 NaN/Inf（降级：SU store OOB 致 addr error）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | scalar_fp_nan_inf |
| 硬件事件编号 | 9 |
| 异常类型 | 标量浮点输入 NaN/Inf（降级：SU store OOB 致 addr error） |
| 错误码 | errcode:TBD addr error 类（验证阶段回填） |
| 类别 | scalar_vec_compute |
| 源文件 | error/main.asc:18-19（`volatile float nanVal = x[0]; addResult = nanVal + 1.0f`）+ :21-22（`*oobPtr = addResult`） |
| 报错模式 | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| 内存区域 | UB（buf+OOB_OFFSET=65536，偏移 256KB 远超声明 1KB） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 双注入（NaN 输入 + SU store OOB）→ 正常输入 + 合法 store + return 1→0 |
| behavior_note | 语义意图对 NaN 输入执行 `nanVal + 1.0f`（静默传播 NaN）；可靠后盾 `*oobPtr=addResult` SU store 到 OOB UB 地址触发 EZ9999 |

## 触发条件

**违规模式**：双路径设计——① 语义意图：对 NaN 输入执行 `nanVal + 1.0f`（发射 SU fp add 指令，输入 NaN，静默传播 NaN）；② 可靠后盾：`*oobPtr = addResult`（SU scalar store 到 OOB UB 地址 buf+65536，偏移 256KB 远超 UB 声明 1KB）触发 EZ9999。

**安全模式**：`buf[0] = addResult;`（合法 store）；`volatile float normalVal = 1.0f`（正常浮点运算，1.0+1.0=2.0）。

## 检测规则

### scalar_fp_nan_inf-RULE-01：浮点 NaN 输入运算（语义意图）

- 【条件】`volatile float nanVal = x[0]`（NaN），执行 `nanVal + 1.0f`
- 【API】`volatile float nanVal = x[0]; volatile float addResult = nanVal + 1.0f;`（error 第 18-19 行）
- 【约束】NaN 输入（L115 EventID 空），静默传播 NaN
- 【匹配模式】diff_pattern

### scalar_fp_nan_inf-RULE-02：SU store OOB（可靠后盾）

- 【条件】`__ubuf__ float* oobPtr = buf + OOB_OFFSET`（OOB_OFFSET=65536），`*oobPtr = addResult`
- 【API】`__ubuf__ float* oobPtr = buf + OOB_OFFSET; *oobPtr = addResult;`（error 第 21-22 行）
- 【约束】oobPtr=buf+65536 偏移 256KB ≫ UB 声明 1KB → SU store 越界 → addr error（L114）
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：NaN 输入改正常值，OOB store 改合法 store。

```cpp
// 修复前（error）
volatile float nanVal = x[0];
volatile float addResult = nanVal + 1.0f;
__ubuf__ float* oobPtr = buf + OOB_OFFSET;
*oobPtr = addResult;

// 修复后（fixed）
volatile float normalVal = 1.0f;
volatile float addResult = 0.0f;
addResult = normalVal + 1.0f;
buf[0] = addResult;
```

**防御性编程建议**：

- 浮点运算前检查输入非 NaN：`if (isnan(val)) val = 0.0f;`
- 禁止 SU store 到 UB 声明范围外

## 变体索引

| 变体 | 输入 | store 目标 | 结果 |
|------|------|-----------|------|
| 基准（本用例） | NaN | buf+65536（OOB） | SU store OOB 致 addr error |
| 纯语义 | NaN | buf[0]（合法） | 静默传播 NaN（无 EZ9999） |
| Inf 输入 | Inf | buf+65536（OOB） | 同族（L115 NaN/Inf） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：浮点运算安全 | 正常输入 + 合法 store | fixed 版 |
| EC2：NaN 静默传播 | NaN 输入 + 合法 store | 纯语义变体（无 EZ9999） |
| EC3：SU store OOB | 任意输入 + OOB store | error 版（可靠触发） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| scalar_fp_neg_sqrt (7) | 同族 SU fp 计算异常，负数 sqrt 路径 |
| scalar_fp_div0 (8) | 同族 SU fp 计算异常，除零路径 |
| vec_nan_inf (4) | 同族 NaN/Inf 输入，VEC 计算路径（count=65535 OOB） |

## 检测执行指引

- **适用匹配模式**：diff_pattern（RULE-01/RULE-02）
- **命中判定**：RULE-01 对应 NaN 语义输入，RULE-02 对应 SU store 越界；仅出现 NaN 输入不能据此确认 EZ9999，纯语义变体静默传播 NaN。
  - 读取 error/main.asc 第 18 行 `nanVal = x[0]`（NaN 输入），第 21-22 行 `oobPtr = buf + OOB_OFFSET; *oobPtr = addResult`（SU store OOB）。
  - 判定 oobPtr=buf+65536 偏移 256KB ≫ UB 声明 1KB → SU store 越界。
  - error 版 stderr 应含 EZ9999（验证阶段确认 errcode，L114 addr error 兜底）。
- **修复映射**：
  - scalar_fp_nan_inf-RULE-01 → 将 NaN 输入改为正常值，种子 fixed 第 18 行为 `normalVal = 1.0f`。
  - scalar_fp_nan_inf-RULE-02 → 将 OOB store 改为声明范围内的 store，种子 fixed 第 22 行为 `buf[0] = addResult`。
