# scalar_int_neg_sqrt — int SQRT 负数输入（降级：SU store OOB 致 addr error）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | scalar_int_neg_sqrt |
| 硬件事件编号 | 6 |
| 异常类型 | int SQRT 负数输入（降级：SU store OOB 致 addr error） |
| 错误码 | errcode:TBD addr error 类（验证阶段回填） |
| 类别 | scalar_vec_compute |
| 源文件 | error/main.asc:18（`volatile int32_t negVal = -16`）+ :22-23（`*oobPtr = sqrtResult`） |
| 报错模式 | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| 内存区域 | UB（buf+OOB_OFFSET=65536，偏移 256KB 远超声明 1KB） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 双注入（负数 int 输入 + SU store OOB）→ 正数 int 输入 + 合法 store + return 1→0 |
| behavior_note | 语义意图对 -16 执行 int sqrt 占位计算（TC-Low 无 int sqrt 标量 API）；可靠后盾 `*oobPtr=sqrtResult` SU store 到 OOB UB 地址触发 EZ9999 |

## 触发条件

**违规模式**：双路径设计——① 语义意图：对负数输入 -16 执行 int sqrt 占位计算（TC-Low `scalar_compute.h` 无 int sqrt 标量 API，硬件 int sqrt 指令的 C 语言发射方式不确定，使用占位计算 `negVal < 0 ? 0 : negVal` exercise SU 计算单元路径）；② 可靠后盾：`*oobPtr = sqrtResult`（SU scalar store 到 OOB UB 地址 buf+65536，偏移 256KB 远超 UB 声明 1KB）触发 EZ9999。

**安全模式**：`buf[0] = sqrtResult;`（合法 store）；`volatile int32_t posVal = 16`（正数输入）。

## 检测规则

### scalar_int_neg_sqrt-RULE-01：负数 int sqrt 输入（语义意图）

- 【条件】`volatile int32_t negVal = -16`，对负数执行 int sqrt 占位计算
- 【API】`volatile int32_t negVal = -16; sqrtResult = negVal < 0 ? 0 : negVal;`（error 第 18, 20 行）
- 【约束】负数 int sqrt 输入（L117 无屏蔽备注），TC-Low 无 int sqrt 标量 API
- 【匹配模式】diff_pattern

### scalar_int_neg_sqrt-RULE-02：SU store OOB（可靠后盾）

- 【条件】`__ubuf__ int32_t* oobPtr = buf + OOB_OFFSET`（OOB_OFFSET=65536），`*oobPtr = sqrtResult`
- 【API】`__ubuf__ int32_t* oobPtr = buf + OOB_OFFSET; *oobPtr = sqrtResult;`（error 第 22-23 行）
- 【约束】oobPtr=buf+65536 偏移 256KB ≫ UB 声明 1KB → SU store 越界 → addr error（L114）
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：负数输入改正数，OOB store 改合法 store。

```cpp
// 修复前（error）
volatile int32_t negVal = -16;
int32_t sqrtResult = 0;
sqrtResult = negVal < 0 ? 0 : negVal;
__ubuf__ int32_t* oobPtr = buf + OOB_OFFSET;
*oobPtr = sqrtResult;

// 修复后（fixed）
volatile int32_t posVal = 16;
int32_t sqrtResult = 0;
sqrtResult = posVal < 0 ? 0 : posVal;
buf[0] = sqrtResult;
```

**防御性编程建议**：

- 对 int sqrt 输入做非负检查：`if (val < 0) val = 0;`
- 禁止 SU store 到 UB 声明范围外

## 变体索引

| 变体 | 负数输入 | store 目标 | 结果 |
|------|---------|-----------|------|
| 基准（本用例） | -16 | buf+65536（OOB） | SU store OOB 致 addr error |
| 纯语义 | -16 | buf[0]（合法） | 占位计算（无 EZ9999） |
| 回退 | -16 | asc_copy_gm2ub_sync OOB | L131 VEC/MTE UB OOB |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：int sqrt 安全 | 正数输入 + 合法 store | fixed 版 |
| EC2：负数 int sqrt 占位 | 负数输入 + 合法 store | 纯语义变体（无 EZ9999） |
| EC3：SU store OOB | 任意输入 + OOB store | error 版（可靠触发） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| scalar_fp_neg_sqrt (7) | 同族 sqrt 负数，float 路径 |
| scalar_fp_div0 (8) | 同族 SU 计算异常，除零路径 |
| trap_instr (L98) | OOB 兜底机制复用本用例验证的 ADDR_MISALIGN 路径 |

## 检测执行指引

- **适用匹配模式**：diff_pattern（RULE-01/RULE-02）
- **命中判定**：RULE-01 对应负数输入的 int sqrt 占位计算，TC-Low 无 int sqrt 标量 API；RULE-02 对应 SU store 越界，纯占位计算变体无 EZ9999。
  - 读取 error/main.asc 第 18 行 `negVal = -16`（负数 int 输入），第 22-23 行 `oobPtr = buf + OOB_OFFSET; *oobPtr = sqrtResult`（SU store OOB）。
  - 判定 oobPtr=buf+65536 偏移 256KB ≫ UB 声明 1KB → SU store 越界。
  - error 版 stderr 应含 EZ9999（验证阶段确认 errcode，L114 addr error 兜底）。
- **修复映射**：
  - scalar_int_neg_sqrt-RULE-01 → 将负数输入改为正数，种子 fixed 第 18 行为 `posVal = 16`。
  - scalar_int_neg_sqrt-RULE-02 → 将 OOB store 改为声明范围内的 store，种子 fixed 第 22 行为 `buf[0] = sqrtResult`。
