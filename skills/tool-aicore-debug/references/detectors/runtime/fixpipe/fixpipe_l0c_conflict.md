# fixpipe_l0c_conflict — FixPipe 指令非法配置（dstStride=0 + nSize>16，L77 路径）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | fixpipe_l0c_conflict |
| 硬件事件编号 | 16 |
| 异常类型 | FixPipe 指令非法配置（dstStride=0 + nSize>16，non nz2nd 路径） |
| 错误码 | errcode:TBD fixp 非法配置类（验证阶段回填） |
| 类别 | fixpipe |
| 源文件 | error/main.asc:83（`fp.dstStride = 0`） |
| 报错模式 | EZ9999 + retCode=0x26 [aicore exception] + aic error mask=0x6500020bd00028c + subErrType=4 |
| 内存区域 | L0C/CO1 → GM（FixPipe 写目标，dstStride=0 致 GM 写址非法） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 常量注入（dstStride 0→n）+ return 1→0 |
| behavior_note | dstStride=0 + nSize=n=32>16（ndNum=1 即非 nz2nd 模式，dst_d=0）致 Fixpipe 指令参数非法 → L77 "non nz2nd n>16 dst_d=0"；完整 TQue 同步保留 |

## 触发条件

**违规模式**：完整 Cube 流水线（Nd2Nz→LoadData2D→Mmad→Fixpipe），完整 TQue 同步（EnQue/DeQue）均保留。Fixpipe 阶段设 `fp.dstStride = 0`（nSize=n=32>16，ndNum=1 即非 nz2nd 模式，dst_d=0）使 Fixpipe 指令参数非法，触发 AICoreError.md L77 "non nz2nd n>16 dst_d=0" 非法配置检测。

**安全模式**：`fp.dstStride = n`（即 32），合法值，与 W5a fixed 一致。

## 检测规则

### fixpipe_l0c_conflict-RULE-01：FixpipeParamsV220.dstStride=0 + nSize>16 致指令非法

- 【条件】非 nz2nd 模式（ndNum=1）下，dstStride=0 且 nSize>16，违反 AICoreError.md L77 "non nz2nd n>16 dst_d=0" 约束
- 【API】`fp.dstStride = 0;`（error）vs `fp.dstStride = n;`（fixed）
- 【约束】ndNum=1（非 nz2nd），nSize=n=32>16，dstStride=dst_d=0 → L77 fixp 指令非法配置
- 【匹配模式】diff_pattern

### fixpipe_l0c_conflict-RULE-02：dstStride 常量注入

- 【条件】`fp.dstStride` 赋值为 0（非法），而非合法的 n 值
- 【API】`fp.dstStride = 0;`（error 第 83 行）vs `fp.dstStride = n;`（fixed 第 83 行）
- 【约束】dstStride 应等于 n（输出列 stride）；dstStride=0 在非 nz2nd n>16 场景下非法
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：将 dstStride 从 0 恢复为合法值 n（即 32）。

```cpp
// 修复前（error）
fp.nSize = n; fp.mSize = m;
fp.srcStride = m;
fp.dstStride = 0;
fp.ndNum = 1; fp.srcNdStride = 0; fp.dstNdStride = 0;
Fixpipe(cGM, c1d, fp);

// 修复后（fixed）
fp.nSize = n; fp.mSize = m;
fp.srcStride = m;
fp.dstStride = n;
fp.ndNum = 1; fp.srcNdStride = 0; fp.dstNdStride = 0;
Fixpipe(cGM, c1d, fp);
```

**防御性编程建议**：

- 非 nz2nd 模式（ndNum=1）下禁止 dstStride=0（nSize>16 时非法）
- dstStride 应等于 n（输出列 stride），与 GM 输出布局一致

## 变体索引

| 变体 | dstStride | nSize | 模式 | 状态 |
|------|-----------|-------|------|------|
| 基准（本用例） | 0 | 32(>16) | non nz2nd | 非法（L77） |
| 安全变体 | n(32) | 32 | non nz2nd | 安全（fixed 版） |
| nz2nd 变体 | 0 | ≤16 | nz2nd | 合法（dst_d=0 允许） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：dstStride 合法 | 非 nz2nd 下 dstStride=n（n>0） | fixed 版（dstStride=n=32） |
| EC2：dstStride=0 非法 | 非 nz2nd + nSize>16 + dstStride=0 | error 版（L77） |
| EC3：nz2nd dstStride=0 合法 | nz2nd 模式下 dst_d=0 允许 | 非本用例范围 |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| fixp_b4_misalign (L77 尝试) | 同族 L77 fixp 非法配置，unitFlag 子条件路径（本用例为 dstStride 子条件） |
| fixp_ch_split_illegal (L77 尝试) | 同族 L77 fixp 非法配置，isChannelSplit 子条件路径 |
| fixp_l0c_rw_conflict (L74 尝试) | 同族 Fixpipe 流水线，L74 冲突路径（L78 兜底） |

## 检测执行指引

- **适用匹配模式**：diff_pattern（`fixpipe_l0c_conflict-RULE-01`）；diff_pattern（`fixpipe_l0c_conflict-RULE-02`）。
- **命中判定**：按 RULE-01 同时核对 ndNum=1、nSize>16 和 dstStride=0 的种子条件，按 RULE-02 对照 dstStride 赋值差异；保留完整 TQue 同步，按本文的 L77 非法配置路径解释结果。保留以下参数、修复对照和日志核对步骤：

  1. 读取 error/main.asc 第 83 行 `fp.dstStride = 0`。
  2. 确认 nSize=n=32>16，ndNum=1（非 nz2nd 模式）→ dst_d=0 非法 → L77。
  3. fixed 版第 83 行 `fp.dstStride = n`（=32）→ 合法。
  4. error 版 stderr 应含 EZ9999 + retCode=0x26 + subErrType=4。
- **修复映射**：
  - `fixpipe_l0c_conflict-RULE-01`、`fixpipe_l0c_conflict-RULE-02` → 将 `fp.dstStride = 0` 恢复为 `fp.dstStride = n`（种子中为 32），使其与 GM 输出布局一致。
