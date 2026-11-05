# cube_fixp_overflow — FixPipe L0C 读越界（mSize 注入路径）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | cube_fixp_overflow |
| 硬件事件编号 | 17（L78 mSize 参数变体） |
| 异常类型 | FixPipe L0C 读地址溢出（mSize M 方向注入路径） |
| 错误码 | errcode:(0, 0x4000000000, 0) The read address of the L0C is out of range |
| 类别 | cube |
| 源文件 | error/main.asc:81（`fp.mSize = m * 128`） |
| 报错模式 | EZ9999 + retCode=0x26 [aicore exception] + subErrType=4 |
| 内存区域 | L0C（Fixpipe 读取源，`__cc__` CO1） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | API 参数注入（fp.mSize: m*128→m）+ return 1→0 |
| behavior_note | mSize=m*128=4096 与种子 Mmad 输出 m=32 不匹配；实际读跨度须按布局和分配验证 |

## 触发条件

> **容量适用范围**：本文容量数值记录历史种子环境，不代表所有芯片或 tensor 的可访问容量。当前输入须先确认架构、API 重载及实际分配边界；未知时标记 `runtime_required=true`。统一单位与计分口径见 `references/constraints/semantic_contract.md`。

**违规模式**：完整 Cube 流水线（Nd2Nz→LoadData2D→Mmad→Fixpipe）中，Fixpipe 阶段 `fp.mSize = m * 128`（即 32×128=4096）使 FixPipe 从 L0C 读取 M 方向 4096 行数据，远超 Mmad 实际写入的 m=32 行。该差异说明请求的 M 范围超出种子生成的有效矩阵；是否越过 L0C 分配以及是否上报 L78，须结合源布局、stride、分配边界和日志确认。历史简化跨度公式不作为物理容量依据。

**安全模式**：`fp.mSize = m`（即 32），与 Mmad 输出 m=32 匹配，合法。

## 检测规则

### cube_fixp_overflow-RULE-01：Fixpipe mSize 远超 Mmad 输出 m

- 【条件】FixpipeParamsV220.mSize > Mmad 实际写入的 m 行数
- 【API】`FixpipeParamsV220 fp; fp.mSize = m * 128;`（mSize=4096 vs Mmad m=32）
- 【约束】本种子请求完整矩阵时 mSize 应等于 Mmad 的 m=32；其他代码允许读取已生成输出的合法子范围，不能仅因不相等就判越界
- 【匹配模式】值域判断

### cube_fixp_overflow-RULE-02：Fixpipe M 方向读取范围超 L0C 容量

- 【条件】依据实际布局换算的 Fixpipe 源读取范围超过已确认的 L0C 分配边界
- 【API】`Fixpipe(cGM, c1d, fp)`；fp.mSize=4096
- 【约束】所有源读取字节区间须在已分配且已生成的输出范围内。40960 个 float32 等于 160KiB，但该数量不是已确认的本种子容量；未知布局/分配时 runtime_required=true
- 【匹配模式】值域判断

## 修复方案

**标准修复模式**：将 fp.mSize 设为与 Mmad 输出 m 一致。

```cpp
// 修复前（error）
fp.nSize = n; fp.mSize = m * 128;  // mSize=4096

// 修复后（fixed）
fp.nSize = n; fp.mSize = m;        // mSize=32，与 Mmad m=32 匹配
```

**防御性编程建议**：

- 对本种子完整矩阵输出，核对 mSize=m；读取子矩阵时按实际有效范围验证
- 封装 Fixpipe 调用前边界检查：`assert(fp.mSize == mmad_m && fp.nSize == mmad_n)`

## 变体索引

| 变体 | mSize 表达式 | mSize 值 | 读 M 方向范围 | 越界 |
|------|-------------|---------|--------------|------|
| 基准（本用例） | m*128 | 4096 | 须按布局换算 | 超出种子有效矩阵；硬件归因须验证 |
| 小倍数变体 | m*4 | 128 | 须按布局换算 | 待验证；不能只因倍数较小就认定安全 |
| nSize 变体 | n*128 | 4096 | N 方向 | — |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：mSize 与 Mmad m 匹配 | fp.mSize == mmad_m | fixed 版（32） |
| EC2：mSize 远超 Mmad m | fp.mSize >> mmad_m 且读取范围 > L0C | error 版（4096） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| cube_l0c_overflow (L13) | 同族 L0C 越界，本用例为 Fixpipe 读侧（L78），L13 为 asc_mmad 写侧 |
| cube_fixp_buffer_overflow (L13) | 同族 L0C，loop 累积路径 |

## 检测执行指引

- **适用匹配模式**：值域判断（`cube_fixp_overflow-RULE-01`）；值域判断（`cube_fixp_overflow-RULE-02`）。
- **命中判定**：按 RULE-01 核对种子完整矩阵的 mSize 与 Mmad 输出 m，按 RULE-02 计算当前布局的实际读取区间和分配边界。保留以下取值、计算和日志核对步骤：

  1. 读取 error/main.asc 第 81 行 `fp.mSize = m * 128`（mSize=4096）。
  2. 比较 Mmad 调用第 75 行 `{m, n, k, ...}` 的 m=32；mSize/m = 4096/32 = 128 ≠ 1 → 异常。
  3. 由目标 API 的源布局、srcStride、srcNdStride 等计算实际读取区间，与已分配且已生成的输出范围比较；缺少布局或分配证据时标记 runtime_required=true。
  4. fixed 版第 81 行 `fp.mSize = m`（32）→ mSize/m=1 → 安全。
  5. error 版 stderr 应含 errcode:(0, 0x4000000000, 0) "L0C read address out of range"。
- **修复映射**：
  - `cube_fixp_overflow-RULE-01`、`cube_fixp_overflow-RULE-02` → 将 `fp.mSize = m * 128` 恢复为 `fp.mSize = m`，使种子中的 mSize=32 与 Mmad 输出 m 一致，并核对读取范围。
