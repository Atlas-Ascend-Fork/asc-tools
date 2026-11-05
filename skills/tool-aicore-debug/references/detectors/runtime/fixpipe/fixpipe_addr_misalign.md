# fixpipe_addr_misalign — FixPipe 读 L0C 越界（srcStride 单块路径）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | fixpipe_addr_misalign |
| 硬件事件编号 | 15 |
| 异常类型 | FixPipe 读 L0C 越界（地址类异常，srcStride 越界路径） |
| 错误码 | errcode:(0, 0x4000000000, 0) The read address of the L0C is out of range |
| 类别 | fixpipe |
| 源文件 | error/main.asc:82（`fp.srcStride = m * 100`） |
| 报错模式 | EZ9999 + retCode=0x26 [aicore exception] + aic error mask=0x6500020bd00028c + subErrType=4 |
| 内存区域 | L0C/CO1（FixPipe 读源，dav-2201 CO1 buffer = 1024 floats） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 常量注入（srcStride m*100→m）+ return 1→0 |
| behavior_note | srcStride=3200（m=32×100）远超 1024-float CO1 buffer，单块 Fixpipe 读 L0C 越界 → L78 读地址溢出 |

## 触发条件

**违规模式**：完整 Cube 计算流水线（Nd2Nz→LoadData2D→Mmad→Fixpipe），在 Fixpipe 阶段设 `fp.srcStride = m * 100`（m=32, srcStride=3200）。Fixpipe 从 CO1/L0C 读取时按 srcStride=3200 步进，远超 CO1 的 1024-float 缓冲区 → L0C 读越界（L78）。

**安全模式**：`fp.srcStride = m`（即 32），合理 stride 值，单块读址 [0, 1024) floats 全在 CO1 buffer 内。

## 检测规则

### fixpipe_addr_misalign-RULE-01：FixpipeParamsV220.srcStride 放大致 L0C 读越界

- 【条件】Fixpipe 的 srcStride 值远超 CO1 buffer 容量（1024 floats），单块读址越界
- 【API】`fp.srcStride = m * 100; Fixpipe(cGM, c1d, fp);`
- 【约束】CO1 buffer = 1024 floats（dav-2201）；srcStride=3200 > 1024 → 单块读址 [0, 3200) floats 跨越 CO1 边界 → L0C 读越界
- 【匹配模式】值域判断

### fixpipe_addr_misalign-RULE-02：srcStride 常量倍数注入

- 【条件】`fp.srcStride` 通过 `m * 100` 构造，人为放大 stride
- 【API】`fp.srcStride = m * 100;`（error）vs `fp.srcStride = m;`（fixed）
- 【约束】m=32，m*100=3200 >> m=32；fixed 版 srcStride=32 ≤ 1024 → 安全
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：将 srcStride 恢复为合法值 m（即 32），与 Mmad 输出矩阵行 stride 一致。

```cpp
// 修复前（error）
fp.srcStride = m * 100;
fp.dstStride = n;
fp.ndNum = 1; fp.srcNdStride = 0; fp.dstNdStride = 0;
Fixpipe(cGM, c1d, fp);

// 修复后（fixed）
fp.srcStride = m;
fp.dstStride = n;
fp.ndNum = 1; fp.srcNdStride = 0; fp.dstNdStride = 0;
Fixpipe(cGM, c1d, fp);
```

**防御性编程建议**：

- 设置 srcStride 前断言 `srcStride <= CO1_SIZE_FLOATS`（1024）
- srcStride 应等于 m（Mmad 输出行 stride），禁止人为放大

## 变体索引

| 变体 | srcStride | 读址范围 | 越界状态 |
|------|-----------|---------|---------|
| 基准（本用例） | m*100=3200 | [0, 3200) floats | 越界（>1024） |
| 边界变体 | m=32 | [0, 32) floats | 安全（fixed 版） |
| 累积变体 | m + ndNum=4 srcNdStride=32768 | — | 见 fixpipe_l1_read_overflow |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：srcStride 合法 | srcStride ≤ CO1_SIZE_FLOATS（1024） | fixed 版（srcStride=m=32） |
| EC2：srcStride 单块越界 | srcStride > CO1_SIZE_FLOATS（单块） | error 版（srcStride=3200） |
| EC3：ndNum 累积越界 | ndNum×srcNdStride 累积跨边界 | fixpipe_l1_read_overflow |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| fixpipe_l1_read_overflow (L78) | 同族 L78 L0C 读越界，ndNum 累积路径（本用例为 srcStride 单块路径） |
| fixp_b4_misalign (L78 兜底) | 同族 srcStride OOB 路径，额外注入 unitFlag 主攻 L77 |
| mte_atomic_misalign | 同族地址类异常，MTE AtomicAdd 对齐路径 |

## 检测执行指引

- **适用匹配模式**：值域判断（`fixpipe_addr_misalign-RULE-01`）；diff_pattern（`fixpipe_addr_misalign-RULE-02`）。
- **命中判定**：按 RULE-02 定位 srcStride 倍数注入，按 RULE-01 结合本文 CO1 buffer 容量核对单块 L0C 读越界；按本文实际机制解释结果。保留以下取值、修复对照和日志核对步骤：

  1. 读取 error/main.asc 第 82 行 `fp.srcStride = m * 100`（m=32 → srcStride=3200）。
  2. 计算 srcStride=3200 > CO1 容量 1024 floats → L0C 读越界。
  3. fixed 版第 82 行 `fp.srcStride = m`（srcStride=32）→ 32 ≤ 1024 → 安全。
  4. error 版 stderr 应含 EZ9999 + retCode=0x26 + errcode:(0, 0x4000000000, 0) + subErrType=4。
- **修复映射**：
  - `fixpipe_addr_misalign-RULE-01`、`fixpipe_addr_misalign-RULE-02` → 将 `fp.srcStride = m * 100` 恢复为 `fp.srcStride = m`（种子中为 32），与 Mmad 输出矩阵行 stride 一致。
