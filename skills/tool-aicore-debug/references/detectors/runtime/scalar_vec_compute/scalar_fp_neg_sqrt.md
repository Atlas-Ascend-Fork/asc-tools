# scalar_fp_neg_sqrt — 负数 SQRT 目标未覆盖（当前种子为乘法与 SU store OOB）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | scalar_fp_neg_sqrt |
| 硬件事件编号 | 7 |
| 异常类型 | 目标为 float SQRT 负数输入；当前种子未执行开方，实际违规为 SU store OOB |
| 错误码 | errcode:TBD addr error 类（验证阶段回填） |
| 类别 | scalar_vec_compute |
| 源文件 | error/main.asc:18-19（`volatile float negVal=-1.0f; sqrtResult=negVal*negVal`）+ :21-22（`*oobPtr = sqrtResult`） |
| 报错模式 | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| 内存区域 | UB（buf+OOB_OFFSET=65536，偏移 256KB 远超声明 1KB） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 乘法输入 -1.0f→1.0f（平方结果均为 1.0f）+ OOB store→合法 store + return 1→0 |
| behavior_note | `negVal * negVal` 是乘法，给定 -1.0f 时结果为 1.0f，不构成负数 SQRT 或 NaN 证据；SU store OOB 的运行时错误码仍需验证 |

## 触发条件

**违规模式**：`*oobPtr = sqrtResult` 写入 UB 声明范围外：buf+OOB_OFFSET=65536，对 float 指针为 256KB 偏移，远超声明的 1KB。具体 EZ9999 错误码需结合运行日志确认。

**证据边界**：当前种子的 `negVal * negVal` 不是开方，-1.0f 的平方为 1.0f。变量名 `sqrtResult` 和 `volatile` 修饰均不能证明执行了 SQRT；该种子未覆盖负数开方目标，也不能用 OOB 报错证明开方异常。

**安全模式**：改为 `buf[0] = sqrtResult;`，写入 UB 声明范围内；该乘法输入无需由负数改为正数。

## 检测规则

### scalar_fp_neg_sqrt-RULE-01：运算类型核对（当前种子未覆盖 SQRT）

- 【条件】比对 error/fixed 的实际运算，只有存在真实浮点开方操作且输入为负数时，才具备负数 SQRT 的源码证据；当前种子的乘法不命中
- 【API】`volatile float negVal = -1.0f; volatile float sqrtResult = negVal * negVal;`（error 第 18-19 行）
- 【约束】-1.0f 与 1.0f 的平方结果均为 1.0f；不得将负数乘法、变量名或 OOB 日志认定为负数开方证据
- 【匹配模式】diff_pattern

### scalar_fp_neg_sqrt-RULE-02：SU store OOB（可靠后盾）

- 【条件】`__ubuf__ float* oobPtr = buf + OOB_OFFSET`（OOB_OFFSET=65536），`*oobPtr = sqrtResult`
- 【API】`__ubuf__ float* oobPtr = buf + OOB_OFFSET; *oobPtr = sqrtResult;`（error 第 21-22 行）
- 【约束】oobPtr=buf+65536 偏移 256KB ≫ UB 声明 1KB → SU store 越界 → addr error（L114）
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：仅将 OOB store 改为合法 store，保留原乘法及输入；不以修改输入符号冒充 SQRT 修复。

```cpp
// 修复前（error）
volatile float negVal = -1.0f;
volatile float sqrtResult = negVal * negVal;
__ubuf__ float* oobPtr = buf + OOB_OFFSET;
*oobPtr = sqrtResult;

// 建议修复（仅修正 store，非原始 fixed 种子的逐字复刻）
volatile float negVal = -1.0f;
volatile float sqrtResult = negVal * negVal;
buf[0] = sqrtResult;
```

原始 fixed 种子还将输入改为 `posVal = 1.0f`；两种输入的平方结果相同，这一符号变更不构成负数开方修复。

**防御性编程建议**：

- 核对实际运算与目标异常是否一致；若要验证负数开方，应另行提供真实开方操作及对应运行证据
- 禁止 SU store 到 UB 声明范围外，store 前断言偏移 < 声明大小

## 变体索引

| 变体 | 乘法输入 | store 目标 | 结果 |
|------|---------|-----------|------|
| 基准（本用例） | -1.0f | buf+65536（OOB） | 平方结果 1.0f；SU store OOB，具体 errcode 待运行验证 |
| 仅乘法 | -1.0f | buf[0]（合法） | 平方结果 1.0f，不构成负数 SQRT 或 NaN 证据 |
| 回退 | -1.0f | asc_copy_gm2ub_sync OOB | L131 VEC/MTE UB OOB |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：正数乘法 + 合法写入 | 输入 1.0f，平方结果 1.0f | 原始 fixed 版 |
| EC2：负数乘法 + 合法写入 | 输入 -1.0f，平方结果 1.0f | 仅乘法变体；未覆盖负数 SQRT |
| EC3：SU store OOB | 任意输入 + OOB store | error 版；错误码需运行日志确认 |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| scalar_fp_div0 (8) | 同族 SU fp 计算异常，除零路径 |
| scalar_fp_nan_inf (9) | 同族 SU fp 计算异常，NaN 输入路径 |
| scalar_addr_error (10) | 同族 OOB 兜底机制（buf+65536） |

## 检测执行指引

- **适用匹配模式**：diff_pattern（RULE-01/RULE-02）
- **命中判定**：RULE-01 先核对运算类型，当前种子只有乘法，明确记为未命中负数 SQRT；RULE-02 独立判断 SU store 越界。
  - 读取 error/main.asc 第 18-19 行 `negVal = -1.0f` 与 `negVal * negVal`，确认平方结果为 1.0f；再核对第 21-22 行 `oobPtr = buf + OOB_OFFSET; *oobPtr = sqrtResult`。
  - 判定 oobPtr=buf+65536 偏移 256KB ≫ UB 声明 1KB → SU store 越界。
  - 具体 errcode 与 L114 归因需运行日志确认，不能从 OOB 证据反推 SQRT 命中；证据不足时按 Step 2 的 `runtime_required=true` 口径处理待验证判断。
- **修复映射**：
  - scalar_fp_neg_sqrt-RULE-01 → 当前乘法不命中，无需修改输入符号；若用户代码确有负数开方，再按实际运算的输入域和计算语义制定修复。
  - scalar_fp_neg_sqrt-RULE-02 → 将 OOB store 改为声明范围内的 store，种子 fixed 第 21 行为 `buf[0] = sqrtResult`。
