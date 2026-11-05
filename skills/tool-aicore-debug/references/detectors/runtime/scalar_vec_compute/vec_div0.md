# vec_div0 — VEC Div 除零（降级：count=65535 致 VEC UB OOB）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | vec_div0 |
| 硬件事件编号 | 3（目标 L135，实际覆盖 L131） |
| 异常类型 | VEC Div 除零（降级：count=65535 致 VEC UB OOB） |
| 错误码 | errcode:(0x4000000000000000, 0, 0) VEC instruction error: the ub address out of bounds |
| 类别 | scalar_vec_compute |
| 源文件 | error/main.asc:31（`Div(zL, xL, yL, 65535)`） |
| 报错模式 | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| 内存区域 | UB（LocalTensor，count=65535 远超声明 256 元素） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | count 注入（65535→256）+ host 除数（0.0f→2.0f）+ return 1→0 |
| behavior_note | count=65535 远超 UB tensor 声明 256 元素，repeat0 对除数 0.0f 执行 Div（语义意图，静默 Inf），repeat1+ 越界访问 UB 触发 VEC UB OOB |

## 触发条件

**违规模式**：TC-High kernel class（双输入队列 inQX/inQY）执行 `Div(zL, xL, yL, 65535)`：count=65535 远超 UB tensor 实际分配的 256 元素，repeat0 对除数 0.0f 执行 Div（语义意图，exercise VEC Div 除零路径，硬件静默产生 Inf），repeat1+ 越界访问 UB 触发 VEC UB OOB。计算异常作为语义意图执行，OOB 越界作为可靠后盾保证 EZ9999 必然触发。

**安全模式**：`Div(zL, xL, yL, 256)`（count=256 合法）；host 侧 `std::vector<float> y(256, 2.0f)`（正常除数，Div(1.0, 2.0)=0.5）。

## 检测规则

### vec_div0-RULE-01：Div count 超声明元素数（可靠后盾 OOB）

- 【条件】`Div` 的 count 参数远超 UB tensor 声明的 256 元素
- 【API】`Div(zL, xL, yL, 65535);`（error 第 31 行）vs `Div(zL, xL, yL, 256);`（fixed 第 31 行）
- 【约束】count=65535 > 声明 256 → repeat1+ 越界访问 UB → VEC UB OOB (errcode 0x4000000000000000)
- 【匹配模式】diff_pattern

### vec_div0-RULE-02：Div 除零输入（语义意图）

- 【条件】host 侧除数为 0.0f，VEC Div 对除零执行（静默产生 Inf）
- 【API】host 侧 `std::vector<float> y(256, 0.0f);`（error 第 53 行）vs `y(256, 2.0f);`（fixed 第 53 行）
- 【约束】除零输入（L135 EventID 空），硬件静默产生 Inf
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：count 恢复为声明元素数，除数改正常值。

```cpp
// 修复前（error）
Div(zL, xL, yL, 65535);       // count 超 256 → VEC UB OOB
// host: std::vector<float> y(256, 0.0f);

// 修复后（fixed）
Div(zL, xL, yL, 256);         // count=256 合法
// host: std::vector<float> y(256, 2.0f);
```

**防御性编程建议**：

- VEC API count 参数应 ≤ UB tensor 声明元素数
- 对 Div 除数做非零检查（业务层过滤 0 值）

## 变体索引

| 变体 | count | 除数 | 结果 |
|------|-------|------|------|
| 基准（本用例） | 65535 | 0.0f | VEC UB OOB（repeat1+）+ 除零语义 |
| 纯语义 | 256 | 0.0f | 静默 Inf（无 EZ9999） |
| 纯 OOB | 65535 | 2.0f | VEC UB OOB |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：VEC Div 安全 | count ≤ 256 + 除数非零 | fixed 版 |
| EC2：除零静默 | count ≤ 256 + 除数 0.0f | 纯语义变体（无 EZ9999） |
| EC3：VEC UB OOB | count > 256 | error 版（可靠触发） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| vec_neg_sqrt (1) | 同族 VEC 计算异常，Sqrt 负数路径（count=65535 OOB） |
| vec_ln_nonpositive (2) | 同族 VEC 计算异常，Ln 非正数路径（count=65535 OOB） |
| vec_nan_inf (4) | 同族 VEC 计算异常，NaN 输入路径（count=65535 OOB） |
| scalar_fp_div0 (8) | 同族除零，SCALAR 路径（SU store OOB） |

## 检测执行指引

- **适用匹配模式**：diff_pattern（RULE-01/RULE-02）
- **命中判定**：RULE-01 对应 count 越界，RULE-02 对应除零语义输入；需区分静默 Inf 与 VEC UB OOB，不能仅凭除零输入确认 EZ9999。
  - 读取 error/main.asc 第 31 行 `Div(zL, xL, yL, 65535)`，第 53 行 `y(256, 0.0f)`。
  - 判定 count=65535 > 声明 256 → repeat1+ 越界访问 UB → VEC UB OOB。
  - error 版 stderr 应含 EZ9999 + errcode:(0x4000000000000000, 0, 0) VEC instruction error: the ub address out of bounds。
- **修复映射**：
  - vec_div0-RULE-01 → 将 count 限制在声明元素数内，种子 fixed 第 31 行为 `Div(zL, xL, yL, 256)`。
  - vec_div0-RULE-02 → 将除数改为非零值，种子 fixed 第 53 行为 `y(256, 2.0f)`。
