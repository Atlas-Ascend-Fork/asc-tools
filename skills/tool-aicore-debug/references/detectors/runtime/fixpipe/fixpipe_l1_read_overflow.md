# fixpipe_l1_read_overflow — FixPipe 读 L0C 越界（ndNum×srcNdStride 累积路径）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | fixpipe_l1_read_overflow |
| 硬件事件编号 | L78（需求名 L79，不可达） |
| 异常类型 | FixPipe 读 L0C 越界（ndNum×srcNdStride 累积路径） |
| 错误码 | errcode:TBD（预期 (0, 0x4000000000, 0) L78 族） |
| 类别 | fixpipe |
| 源文件 | error/main.asc:84-86（`fp.ndNum = 4; fp.srcNdStride = 32768; fp.dstNdStride = n;`） |
| 报错模式 | EZ9999 + retCode=0x26 [aicore exception] + aic error mask=0x6500020bd00028c + subErrType=4 |
| 内存区域 | L0C/CO1（FixPipe 读源，dav-2201 CO1 buffer = 1024 floats） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 参数注入（ndNum 4→1, srcNdStride 32768→0, dstNdStride n→0）+ return 1→0 |
| behavior_note | ndNum=4, srcNdStride=32768：第 1 块单独不溢出，第 2/3/4 块 L0C 读地址 32768/65536/98304 floats 远超 1024-float CO1 buffer → L0C 读越界（L78） |

## 触发条件

**违规模式**：完整 Cube 流水线（Nd2Nz→LoadData2D→Mmad→Fixpipe），Fixpipe 阶段保持单块 `srcStride=m`（合法）但设 `ndNum=4, srcNdStride=32768`。第 1 块单独不溢出，溢出完全来自 srcNdStride 累积（第 2/3/4 块 L0C 读地址 32768/65536/98304 floats 远超 1024-float CO1 buffer）→ L0C 读越界（L78）。

**安全模式**：`fp.ndNum = 1; fp.srcNdStride = 0; fp.dstNdStride = 0;`（单块模式，无累积）。

## 检测规则

### fixpipe_l1_read_overflow-RULE-01：FixpipeParamsV220 ndNum×srcNdStride 累积致 L0C 读越界

- 【条件】Fixpipe 的 ndNum > 1 且 srcNdStride 远超 CO1 buffer 容量，多块累积读址越界
- 【API】`fp.ndNum = 4; fp.srcNdStride = 32768; Fixpipe(cGM, c1d, fp);`
- 【约束】CO1 buffer = 1024 floats；第 i 块（i≥1）读地址 = i×srcNdStride = i×32768 >> 1024 → 越界
- 【匹配模式】值域判断

### fixpipe_l1_read_overflow-RULE-02：ndNum/srcNdStride 参数注入

- 【条件】`fp.ndNum = 4`（非默认 1）+ `fp.srcNdStride = 32768`（超大值）
- 【API】`fp.ndNum = 4; fp.srcNdStride = 32768; fp.dstNdStride = n;`（error）vs `fp.ndNum = 1; fp.srcNdStride = 0; fp.dstNdStride = 0;`（fixed）
- 【约束】ndNum=4 时第 2 块起 srcNdStride=32768 累积越界；fixed 版 ndNum=1 无累积 → 安全
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：恢复单块模式（ndNum=1, srcNdStride=0, dstNdStride=0），消除累积越界。

```cpp
// 修复前（error）
fp.srcStride = m;
fp.dstStride = n;
fp.ndNum = 4;
fp.srcNdStride = 32768;
fp.dstNdStride = n;
Fixpipe(cGM, c1d, fp);

// 修复后（fixed）
fp.srcStride = m;
fp.dstStride = n;
fp.ndNum = 1; fp.srcNdStride = 0; fp.dstNdStride = 0;
Fixpipe(cGM, c1d, fp);
```

**防御性编程建议**：

- 设 ndNum > 1 时断言 `(ndNum-1) × srcNdStride + m × n <= CO1_SIZE_FLOATS`（1024）
- srcNdStride 应与实际 CO1 buffer 容量匹配，禁止随意放大

## 变体索引

| 变体 | ndNum | srcNdStride | 累积末址 | 越界状态 |
|------|-------|-------------|---------|---------|
| 基准（本用例） | 4 | 32768 | 98304 floats | 越界（>>1024） |
| 单块变体 | 1 | 0 | 0 | 安全（fixed 版） |
| 单块路径 | 1 | 0 | — | 见 fixpipe_addr_misalign（srcStride 单块） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：ndNum 累积读址安全 | (ndNum-1)×srcNdStride ≤ CO1_SIZE_FLOATS | fixed 版（ndNum=1） |
| EC2：ndNum 累积读址越界 | (ndNum-1)×srcNdStride > CO1_SIZE_FLOATS | error 版（3×32768=98304） |
| EC3：srcStride 单块越界 | srcStride > CO1_SIZE_FLOATS | fixpipe_addr_misalign |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| fixpipe_addr_misalign (15) | 同族 L78 L0C 读越界，srcStride 单块路径（本用例为 ndNum 累积路径） |
| fixp_l1_write_overflow (L78 兜底) | 同族读侧参数放大 L78，含 ndNum+srcNdStride+srcStride 三参数 |
| fixp_l0c_rw_conflict (L78 兜底) | 同族 srcStride OOB 路径，额外移除同步主攻 L74 |

## 检测执行指引

- **适用匹配模式**：值域判断（`fixpipe_l1_read_overflow-RULE-01`）；diff_pattern（`fixpipe_l1_read_overflow-RULE-02`）。
- **命中判定**：按 RULE-01 核对多块 srcNdStride 累积读址，按 RULE-02 对照 ndNum/srcNdStride/dstNdStride 注入差异；保留单块 srcStride=m 合法、第 2 块起累积越界的机制。保留以下参数、修复对照和日志核对步骤：

  1. 读取 error/main.asc 第 84 行 `fp.ndNum = 4`，第 85 行 `fp.srcNdStride = 32768`。
  2. 计算第 2 块起读地址 = 1×32768 = 32768 > CO1 容量 1024 floats → L0C 读越界。
  3. fixed 版第 84 行 `fp.ndNum = 1; fp.srcNdStride = 0`（单块，无累积）→ 安全。
  4. error 版 stderr 应含 EZ9999 + retCode=0x26 + subErrType=4。
- **修复映射**：
  - `fixpipe_l1_read_overflow-RULE-01`、`fixpipe_l1_read_overflow-RULE-02` → 恢复单块参数 `fp.ndNum = 1; fp.srcNdStride = 0; fp.dstNdStride = 0;`，并保留合法的 `srcStride=m`、`dstStride=n`。
