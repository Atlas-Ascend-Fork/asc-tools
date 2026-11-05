# dvg_stack_overflow — DVG STACK overflow（条件分支过多）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | dvg_stack_overflow |
| 硬件事件编号 | L158 |
| 异常类型 | DVG STACK overflow |
| 需求目标 | L158 VEC SIMT DVG stack 溢出（条件分支过多） |
| 实际覆盖 | 32 层宏嵌套条件分支探索 L158（主路径，编译器可能优化或 DVG 检测在编译期）；OOB 兜底 UB 写越界致 ADDR_MISALIGN（scalar_int_neg_sqrt 已验证） |
| 错误码 | errcode:(TBD, TBD, 0)（L158 主路径无先例，OOB 降级 ADDR_MISALIGN 族） |
| 类别 | memory_overflow |
| 源文件 | error/main.asc:10（`OOB_OFFSET = 65536`）+ :13-14（DVG_IF/DVG_END 宏）+ :24-40（32 层嵌套）+ :42-43（OOB 兜底） |
| 报错模式 | EZ9999（验证阶段补充 retCode/errcode/subErrType/vec error info 字段） |
| 内存区域 | DVG stack（SIMT 条件分支压栈）+ UB（OOB 兜底） |
| API层级 | TC-Low |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 结构变更（OOB_OFFSET=65536→0 + host x=100→-1 致 cond 为假不压栈）+ return 1→0 |
| behavior_note | 主路径 32 层宏嵌套 if 压栈探索 L158；OOB 兜底 buf+65536 >> UB 1KB 致 ADDR_MISALIGN |

## 触发条件

**违规模式（主路径）**：宏 `DVG_IF(n)` 展开 32 层深度嵌套条件分支（每层 if 压栈），cond 从 GM 读取（host 填 100，致所有 `cond>n` 为真，32 层全压栈）→ DVG stack 溢出（L158 探索）。不加 `#pragma unroll` 保持嵌套结构防编译器扁平化。

**违规模式（OOB 兜底）**：`__ubuf__ int32_t* oobPtr = buf + OOB_OFFSET; *oobPtr = depth;` — buf+65536 >> UB 声明 1KB → ADDR_MISALIGN（scalar_int_neg_sqrt 已验证必触发 EZ9999）。

**宏展开说明**：`DVG_IF(n)` 展开为 `if (cond > (n)) { depth++;`（不闭合），`DVG_END` 展开为 `}`。32 个 DVG_IF 连写形成 32 层嵌套 `{`，中间 `buf[0]=depth`，32 个 DVG_END 闭合。

**安全模式**：`OOB_OFFSET = 0`（合法 UB 偏移）+ host `x` 填 -1（致 `cond=-1`，所有 `cond>n` 为假，DVG 不压栈，深度 0）。32 层宏结构保留。

## 检测规则

### dvg_stack_overflow-RULE-01：32 层宏嵌套条件分支致 DVG stack 溢出

- 【条件】宏展开 32 层深度嵌套条件分支（每层 if 压栈），cond 为真使全压栈
- 【API】`DVG_IF(0) DVG_IF(1) ... DVG_IF(31)` + `buf[0] = depth;` + `DVG_END × 32`（宏展开 32 层嵌套 `{`）
- 【约束】DVG stack 容量有限；32 层嵌套 if 全压栈（cond=100 > n 对所有 n=0..31 为真）→ DVG stack 溢出 → L158
- 【匹配模式】值域判断

### dvg_stack_overflow-RULE-02：UB 写越界 OOB 兜底（buf+OOB_OFFSET）

- 【条件】`__ubuf__ int32_t* oobPtr = buf + OOB_OFFSET` 使 UB 写地址远超声明大小
- 【API】`__ubuf__ int32_t* oobPtr = buf + OOB_OFFSET; *oobPtr = depth;`（OOB_OFFSET=65536）
- 【约束】buf 声明 TILE_LENGTH=256 int32_t = 1KB；OOB_OFFSET=65536 >> 256 → UB OOB → ADDR_MISALIGN
- 【匹配模式】值域判断

### dvg_stack_overflow-RULE-03：OOB_OFFSET 常量注入 + host cond 控制

- 【条件】OOB_OFFSET 从 65536 改为 0，host x 从 100 改为 -1（致 cond 为假不压栈）
- 【API】`constexpr uint32_t OOB_OFFSET = 65536;`（error）vs `constexpr uint32_t OOB_OFFSET = 0;`（fixed）
- 【约束】error 版 OOB_OFFSET=65536 + host x=100（cond=100 全压栈）；fixed 版 OOB_OFFSET=0 + host x=-1（cond=-1 不压栈）
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：OOB_OFFSET 从 65536 改为 0（合法 UB 偏移）；host `x` 填 -1（致 `cond=-1`，所有 `cond>n` 为假，DVG 不压栈，深度 0）。32 层宏结构保留。

```cpp
// 修复前（error）
constexpr uint32_t OOB_OFFSET = 65536;
// host: std::vector<int32_t> x(totalLength, 100);

// 修复后（fixed）
constexpr uint32_t OOB_OFFSET = 0;
// host: std::vector<int32_t> x(totalLength, -1);
```

**防御性编程建议**：

- 限制条件分支嵌套深度，避免 DVG stack 溢出
- UB 指针偏移前 assert `offset < TILE_LENGTH`
- 条件分支 cond 从 GM 读取时，host 端控制 cond 值避免深层压栈

## 变体索引

| 变体 | OOB_OFFSET | host x | cond | 压栈深度 | 触发路径 |
|------|-----------|--------|------|---------|---------|
| 基准（本用例） | 65536 | 100 | 100 | 32 | L158 + ADDR_MISALIGN |
| 仅主路径 | 0 | 100 | 100 | 32 | L158 |
| 仅 OOB | 65536 | -1 | -1 | 0 | ADDR_MISALIGN |
| 合法变体 | 0 | -1 | -1 | 0 | 安全（fixed 版） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：DVG 不压栈 + UB 合法 | cond 为假（不压栈）且 OOB_OFFSET=0 | fixed 版（cond=-1, OOB=0） |
| EC2：DVG 深层压栈 | 嵌套深度 32 且 cond 为真 | error 版主路径（cond=100, 压栈 32 层） |
| EC3：UB 写越界（OOB 兜底） | OOB_OFFSET > TILE_LENGTH | error 版兜底（65536 > 256） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| scalar_int_neg_sqrt | OOB 兜底路径已验证用例，UB ADDR_MISALIGN |
| atomic_dirty_cacheline | 同族探索类，OOB 兜底结构 |

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01、RULE-02）、diff_pattern（RULE-03）。
- **命中判定**：按下列步骤核对 RULE-01 的分支嵌套深度与 cond、RULE-02 的 UB 偏移和 RULE-03 的 error/fixed 差异；主路径与 OOB 兜底分别记录证据。
  1. 读取 error/main.asc 第 10 行 `OOB_OFFSET = 65536` 确认 OOB 兜底参数。
  2. 读取第 13-14 行 `DVG_IF(n)` / `DVG_END` 宏定义确认嵌套展开规则。
  3. 读取第 24-31 行 32 个 `DVG_IF(n)` 确认 32 层嵌套结构；第 32 行 `buf[0] = depth`；第 33-40 行 32 个 `DVG_END` 闭合。
  4. 读取第 42-43 行 `buf + OOB_OFFSET` + `*oobPtr = depth` 确认 OOB 兜底（65536 >> 256）。
  5. 读取第 57 行 host `x(totalLength, 100)` 确认 cond=100（全压栈）。
  6. fixed 版第 10 行 `OOB_OFFSET = 0` + 第 57 行 `x(totalLength, -1)`（cond=-1 不压栈）→ 合法。
  7. error 版 stderr 应含 EZ9999（验证阶段补充 retCode/errcode/subErrType/vec error info）。
  - `runtime_required=true`：L158 主路径需运行时 retCode/errcode/subErrType/vec error info 确认；仅有 EZ9999 或 OOB 兜底证据不能确认 DVG stack 溢出。
- **修复映射**：RULE-01 → 限制条件分支嵌套深度，种子修复使用 host `x=-1` 使 cond 为假、不压栈；RULE-02 → 将 `OOB_OFFSET` 从 65536 改为 0；RULE-03 → 同时采用上述两项参数修复，保留 32 层宏结构。
