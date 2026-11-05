# fixp_l1_write_overflow — FIXP L0C 读越界（读侧参数同向放大路径，L80 不可达兜底 L78）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | fixp_l1_write_overflow |
| 硬件事件编号 | L80（不可达）/ L78（OOB 兜底） |
| 异常类型 | FIXP L0C 读越界（读侧参数同向放大路径） |
| 错误码 | errcode:TBD（预期 L78 族 (0, 0x4000000000, 0)） |
| 类别 | fixpipe |
| 源文件 | error/main.asc:85-87（`fp.srcStride = OOB_SRC_STRIDE` + `fp.ndNum = ND_NUM` + `fp.srcNdStride = SRC_ND_STRIDE`） |
| 报错模式 | EZ9999 + retCode=0x26 [aicore exception] |
| 内存区域 | L0C/CO1（FixPipe 读源，dav-2201 CO1 buffer = 1024 floats） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 参数注入（srcStride 3200→m + ndNum 255→1 + srcNdStride 32768→0）+ return 1→0 |
| behavior_note | 读侧三参数同向放大（srcStride=3200 + ndNum=255 + srcNdStride=32768）致 L0C 读越界 L78；L80 经三重缺陷证伪不可达（L0C→L1 不支持 float→float / NoQuant 静默空操作 / dstNdStride 被忽略） |

## 触发条件

**违规模式**：`__cube__` TC-High kernel，完整 Cube 流水线（Nd2Nz→LoadData2D→Mmad→Fixpipe，完整 TQue 同步保留）。L0C→GM `Fixpipe(cGM, c1d, fp)` 重载 + 读侧三参数同向放大：`srcStride=OOB_SRC_STRIDE(3200)` + `ndNum=ND_NUM(255)` + `srcNdStride=SRC_ND_STRIDE(32768)` → L0C 读越界 = L78 → EZ9999。

**安全模式**：`fp.srcStride = m; fp.ndNum = 1; fp.srcNdStride = 0; fp.dstNdStride = 0;`（合法参数）。

**L80 不可达说明**：经 CANN 9.0.0 dav-2201 实现三重缺陷证伪：①L0C→L1 不支持 float→float；②NoQuant 在 L0C→L1 switch 中无 case → 静默空操作；③L0C→L1 不调用 SetNdParaImpl，dstNdStride/ndNum/srcNdStride 被忽略。

## 检测规则

### fixp_l1_write_overflow-RULE-01：读侧三参数同向放大致 L0C 读越界

- 【条件】Fixpipe 读侧参数（srcStride + ndNum + srcNdStride）同向放大，单块/累积读址均超 CO1 buffer 容量
- 【API】`fp.srcStride = OOB_SRC_STRIDE; fp.ndNum = ND_NUM; fp.srcNdStride = SRC_ND_STRIDE; Fixpipe(cGM, c1d, fp);`
- 【约束】CO1 buffer = 1024 floats；srcStride=3200（单块越界）+ ndNum=255×srcNdStride=32768（累积越界）→ 三参数均独立致 L0C 读越界
- 【匹配模式】值域判断

### fixp_l1_write_overflow-RULE-02：srcStride + ndNum + srcNdStride 三参数注入

- 【条件】三个读侧参数同时注入异常值
- 【API】error：`fp.srcStride = OOB_SRC_STRIDE; fp.ndNum = ND_NUM; fp.srcNdStride = SRC_ND_STRIDE; fp.dstNdStride = 0;` vs fixed：`fp.srcStride = m; fp.ndNum = 1; fp.srcNdStride = 0; fp.dstNdStride = 0;`
- 【约束】OOB_SRC_STRIDE=3200 > 1024；ND_NUM=255, SRC_ND_STRIDE=32768 → (255-1)×32768 >> 1024；fixed 版全部合法
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：读侧三参数全部恢复合法值（srcStride=m, ndNum=1, srcNdStride=0, dstNdStride=0）。

```cpp
// 修复前（error）
fp.srcStride = OOB_SRC_STRIDE;
fp.dstStride = n;
fp.ndNum = ND_NUM; fp.srcNdStride = SRC_ND_STRIDE; fp.dstNdStride = 0;
Fixpipe(cGM, c1d, fp);

// 修复后（fixed）
fp.srcStride = m;
fp.dstStride = n;
fp.ndNum = 1; fp.srcNdStride = 0; fp.dstNdStride = 0;
Fixpipe(cGM, c1d, fp);
```

**防御性编程建议**：

- 设 ndNum > 1 时断言 `(ndNum-1)×srcNdStride + srcStride <= CO1_SIZE_FLOATS`（1024）
- srcStride 应等于 m（Mmad 输出行 stride），禁止放大

## 变体索引

| 变体 | srcStride | ndNum | srcNdStride | 越界路径 |
|------|-----------|-------|-------------|---------|
| 基准（本用例） | 3200 | 255 | 32768 | 单块 + 累积（三参数同向） |
| 单块变体 | m(32) | 1 | 0 | 安全（fixed 版） |
| 累积变体 | m | 4 | 32768 | 见 fixpipe_l1_read_overflow |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：读侧参数合法 | srcStride≤1024 且 (ndNum-1)×srcNdStride≤1024 | fixed 版 |
| EC2：读侧参数同向越界 | srcStride>1024 或 (ndNum-1)×srcNdStride>1024 | error 版 |
| EC3：L80 写溢出 | L0C→L1 写地址溢出（dav-2201 不可达） | 三重缺陷证伪 |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| fixpipe_l1_read_overflow (L78) | 同族读侧 ndNum 累积 L78，主要重复源（ndNum=255 vs 4） |
| fixpipe_addr_misalign (15) | 同族 srcStride 单块 L78，次要重复源 |
| fixp_l0c_rw_conflict (L78 兜底) | 同族 srcStride OOB 路径，额外移除同步主攻 L74 |

## 检测执行指引

- **适用匹配模式**：值域判断（`fixp_l1_write_overflow-RULE-01`）；diff_pattern（`fixp_l1_write_overflow-RULE-02`）。
- **命中判定**：按 RULE-01 分别核对 srcStride 单块越界与 `(ND_NUM-1)×SRC_ND_STRIDE` 累积越界，按 RULE-02 对照三个读侧参数的注入差异；保留本文 CANN 9.0.0 dav-2201 上 L80 不可达、实际检查 L78 读侧越界的限制。保留以下取值、修复对照和重复覆盖判别步骤：

  1. 读取 error/main.asc 第 10 行 `OOB_SRC_STRIDE = 3200`，第 11 行 `ND_NUM = 255`，第 12 行 `SRC_ND_STRIDE = 32768`。
  2. 读取第 85-87 行 `fp.srcStride = OOB_SRC_STRIDE; fp.ndNum = ND_NUM; fp.srcNdStride = SRC_ND_STRIDE`。
  3. 计算 srcStride=3200 > CO1 容量 1024 floats → L0C 读越界（OOB 兜底 L78）。
  4. fixed 版 srcStride=m, ndNum=1, srcNdStride=0 → 安全。
  5. error 版 stderr 应含 EZ9999 + retCode=0x26。
  6. 签名判别：若与 fixpipe_l1_read_overflow/fixpipe_addr_misalign 整体一致 → 标 DUPLICATE_COVERAGE。
- **修复映射**：
  - `fixp_l1_write_overflow-RULE-01`、`fixp_l1_write_overflow-RULE-02` → 恢复 `srcStride=m`、`ndNum=1`、`srcNdStride=0`、`dstNdStride=0`，消除单块和累积两条读侧越界路径；按原修复方案保留 `dstStride=n` 和完整同步。
