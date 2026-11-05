# scalar_fp_div0 — float 除零（降级：SU store OOB 致 addr error）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | scalar_fp_div0 |
| 硬件事件编号 | 8 |
| 异常类型 | float 除零（降级：SU store OOB 致 addr error） |
| 错误码 | errcode:TBD addr error 类（验证阶段回填） |
| 类别 | scalar_vec_compute |
| 源文件 | error/main.asc:19（`volatile float denominator = 0.0f`）+ :23-24（`*oobPtr = divResult`） |
| 报错模式 | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| 内存区域 | UB（buf+OOB_OFFSET=65536，偏移 256KB 远超声明 1KB） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 双注入（除数 0.0f + SU store OOB）→ 除数 2.0f + 合法 store + return 1→0 |
| behavior_note | 语义意图 1.0f/0.0f 除零（静默产生 Inf，volatile 防常量折叠）；可靠后盾 `*oobPtr=divResult` SU store 到 OOB UB 地址触发 EZ9999 |

## 触发条件

**违规模式**：双路径设计——① 语义意图：对除数 0.0f 执行 `numerator / denominator`（1.0f/0.0f，发射 SU fp div 指令，文档建议软件屏蔽，静默产生 Inf，`volatile` 防止编译器常量折叠）；② 可靠后盾：`*oobPtr = divResult`（SU scalar store 到 OOB UB 地址 buf+65536，偏移 256KB 远超 UB 声明 1KB）触发 EZ9999。

**安全模式**：`buf[0] = divResult;`（合法 store）；`volatile float denominator = 2.0f`（正常除数，1.0/2.0=0.5）。

## 检测规则

### scalar_fp_div0-RULE-01：浮点除零输入（语义意图）

- 【条件】`volatile float denominator = 0.0f`，执行 `numerator / denominator`（1.0f/0.0f）
- 【API】`volatile float denominator = 0.0f; divResult = numerator / denominator;`（error 第 19, 21 行）
- 【约束】除数为 0.0f，文档建议软件屏蔽（L110），静默产生 Inf
- 【匹配模式】diff_pattern

### scalar_fp_div0-RULE-02：SU store OOB（可靠后盾）

- 【条件】`__ubuf__ float* oobPtr = buf + OOB_OFFSET`（OOB_OFFSET=65536），`*oobPtr = divResult`
- 【API】`__ubuf__ float* oobPtr = buf + OOB_OFFSET; *oobPtr = divResult;`（error 第 23-24 行）
- 【约束】oobPtr=buf+65536 偏移 256KB ≫ UB 声明 1KB → SU store 越界 → addr error（L114）
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：除数 0.0f 改正常值，OOB store 改合法 store。

```cpp
// 修复前（error）
volatile float denominator = 0.0f;
divResult = numerator / denominator;
__ubuf__ float* oobPtr = buf + OOB_OFFSET;
*oobPtr = divResult;

// 修复后（fixed）
volatile float denominator = 2.0f;
divResult = numerator / denominator;
buf[0] = divResult;
```

**防御性编程建议**：

- 除法前检查除数非零：`if (denominator == 0.0f) denominator = epsilon;`
- 禁止 SU store 到 UB 声明范围外

## 变体索引

| 变体 | 除数 | store 目标 | 结果 |
|------|------|-----------|------|
| 基准（本用例） | 0.0f | buf+65536（OOB） | SU store OOB 致 addr error |
| 纯语义 | 0.0f | buf[0]（合法） | 静默 Inf（无 EZ9999） |
| 回退 | 0.0f | asc_copy_gm2ub_sync OOB | L131 VEC/MTE UB OOB |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：除法安全 | 除数非零 + 合法 store | fixed 版 |
| EC2：除零静默 | 除数 0.0f + 合法 store | 纯语义变体（无 EZ9999） |
| EC3：SU store OOB | 任意除数 + OOB store | error 版（可靠触发） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| scalar_fp_neg_sqrt (7) | 同族 SU fp 计算异常，负数 sqrt 路径 |
| scalar_fp_nan_inf (9) | 同族 SU fp 计算异常，NaN 输入路径 |
| vec_div0 (3) | 同族除零，VEC 计算路径（count=65535 OOB） |

## 检测执行指引

- **适用匹配模式**：diff_pattern（RULE-01/RULE-02）
- **命中判定**：RULE-01 对应除零语义输入，RULE-02 对应 SU store 越界；仅出现除零输入不能据此确认 EZ9999，纯语义变体静默产生 Inf。
  - 读取 error/main.asc 第 19 行 `denominator = 0.0f`（除零），第 23-24 行 `oobPtr = buf + OOB_OFFSET; *oobPtr = divResult`（SU store OOB）。
  - 判定 oobPtr=buf+65536 偏移 256KB ≫ UB 声明 1KB → SU store 越界。
  - error 版 stderr 应含 EZ9999（验证阶段确认 errcode，L114 addr error 兜底）。
- **修复映射**：
  - scalar_fp_div0-RULE-01 → 将除数 `0.0f` 改为正常值，种子 fixed 第 19 行为 `denominator = 2.0f`。
  - scalar_fp_div0-RULE-02 → 将 OOB store 改为声明范围内的 store，种子 fixed 第 23 行为 `buf[0] = divResult`。
