# vec_nan_inf — VEC 输入 NaN/Inf（降级：count=65535 致 VEC UB OOB）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | vec_nan_inf |
| 硬件事件编号 | 4（目标 L136，实际覆盖 L131） |
| 异常类型 | VEC 输入 NaN/Inf（降级：count=65535 致 VEC UB OOB） |
| 错误码 | errcode:(0x4000000000000000, 0, 0) VEC instruction error: the ub address out of bounds |
| 类别 | scalar_vec_compute |
| 源文件 | error/main.asc:27（`Muls(zL, xL, (float)2.0, 65535)`）+ :47-50（NaN 构造） |
| 报错模式 | EZ9999（验证阶段回填 retCode/exit_code/errcode） |
| 内存区域 | UB（LocalTensor，count=65535 远超声明 256 元素） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | count 注入（65535→256）+ host NaN 构造→正常输入 + `#include <cstring>` 移除 + return 1→0 |
| behavior_note | count=65535 远超 UB tensor 声明 256 元素，repeat0 对 NaN 输入执行 Muls（语义意图，静默传播 NaN），repeat1+ 越界访问 UB 触发 VEC UB OOB |

## 触发条件

**违规模式**：TC-High kernel class（Muls API + NaN 输入）执行 `Muls(zL, xL, (float)2.0, 65535)`：count=65535 远超 UB tensor 实际分配的 256 元素，repeat0 对 NaN 输入执行 Muls（语义意图，exercise VEC 计算单元 NaN 传播路径，硬件静默传播 NaN），repeat1+ 越界访问 UB 触发 VEC UB OOB。与 Seed-V3 区别：Seed-V3 使用正常输入(1.0)覆盖纯地址越界，本用例使用 NaN 输入填补 VEC 计算异常类（L136）语义空白。

**安全模式**：`Muls(zL, xL, (float)2.0, 256)`（count=256 合法）；host 侧 `std::vector<float> x(256, 1.0f)`（正常输入，Muls(1.0, 2.0)=2.0）。

## 检测规则

### vec_nan_inf-RULE-01：Muls count 超声明元素数（可靠后盾 OOB）

- 【条件】`Muls` 的 count 参数远超 UB tensor 声明的 256 元素
- 【API】`Muls(zL, xL, (float)2.0, 65535);`（error 第 27 行）vs `Muls(zL, xL, (float)2.0, 256);`（fixed 第 26 行）
- 【约束】count=65535 > 声明 256 → repeat1+ 越界访问 UB → VEC UB OOB (errcode 0x4000000000000000)
- 【匹配模式】diff_pattern

### vec_nan_inf-RULE-02：NaN 输入构造（语义意图）

- 【条件】host 侧构造 IEEE 754 quiet NaN（0x7FC00000）作为 VEC 输入
- 【API】host 侧 `uint32_t nanBits = 0x7FC00000; float nanVal; std::memcpy(&nanVal, &nanBits, sizeof(float)); std::vector<float> x(256, nanVal);`（error 第 47-50 行）vs `x(256, 1.0f);`（fixed 第 46 行）
- 【约束】NaN 输入（L136 EventID 空），硬件静默传播 NaN
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：count 恢复为声明元素数，NaN 输入改正常值。

```cpp
// 修复前（error）
Muls(zL, xL, (float)2.0, 65535);   // count 超 256 → VEC UB OOB
// host:
uint32_t nanBits = 0x7FC00000;
float nanVal;
std::memcpy(&nanVal, &nanBits, sizeof(float));
std::vector<float> x(256, nanVal);

// 修复后（fixed）
Muls(zL, xL, (float)2.0, 256);     // count=256 合法
// host: std::vector<float> x(256, 1.0f);
```

**防御性编程建议**：

- VEC API count 参数应 ≤ UB tensor 声明元素数
- 对 VEC 输入做 NaN/Inf 检查（业务层过滤异常值）

## 变体索引

| 变体 | count | 输入 | 结果 |
|------|-------|------|------|
| 基准（本用例） | 65535 | NaN (0x7FC00000) | VEC UB OOB（repeat1+）+ NaN 传播语义 |
| 纯语义 | 256 | NaN | 静默传播 NaN（无 EZ9999） |
| 纯 OOB (Seed-V3) | 65535 | 1.0f | VEC UB OOB |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：VEC Muls 安全 | count ≤ 256 + 正常输入 | fixed 版 |
| EC2：NaN 静默传播 | count ≤ 256 + NaN 输入 | 纯语义变体（无 EZ9999） |
| EC3：VEC UB OOB | count > 256 | error 版（可靠触发） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| vec_neg_sqrt (1) | 同族 VEC 计算异常，Sqrt 负数路径（count=65535 OOB） |
| vec_ln_nonpositive (2) | 同族 VEC 计算异常，Ln 非正数路径（count=65535 OOB） |
| vec_div0 (3) | 同族 VEC 计算异常，Div 除零路径（count=65535 OOB） |
| scalar_fp_nan_inf (9) | 同族 NaN/Inf 输入，SCALAR 路径（SU store OOB） |

## 检测执行指引

- **适用匹配模式**：diff_pattern（RULE-01/RULE-02）
- **命中判定**：RULE-01 对应 count 越界，RULE-02 对应 NaN 输入构造；需区分静默 NaN 传播与 VEC UB OOB。
  - 读取 error/main.asc 第 27 行 `Muls(zL, xL, (float)2.0, 65535)`，第 47-50 行 NaN 构造（nanBits=0x7FC00000）。
  - 判定 count=65535 > 声明 256 → repeat1+ 越界访问 UB → VEC UB OOB。
  - error 版 stderr 应含 EZ9999 + errcode:(0x4000000000000000, 0, 0) VEC instruction error: the ub address out of bounds。
- **修复映射**：
  - vec_nan_inf-RULE-01 → 将 count 限制在声明元素数内，种子 fixed 第 26 行为 `Muls(zL, xL, (float)2.0, 256)`。
  - vec_nan_inf-RULE-02 → 将 NaN 输入改为正常值，种子 fixed 第 46 行为 `x(256, 1.0f)`。
