# fixp_b4_misalign — FixPipe b4 输出 N 非 64 对齐（unitFlag 注入路径，OOB 兜底 L78）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | fixp_b4_misalign |
| 硬件事件编号 | L77（尝试）/ L78（OOB 兜底，实际覆盖） |
| 异常类型 | FixPipe b4 输出 N 非 64 对齐（fixp 非法配置，unitFlag 路径） |
| 错误码 | errcode:(0, 0x4000000000, 0) The read address of the L0C is out of range（OOB 兜底 L78） |
| 类别 | fixpipe |
| 源文件 | error/main.asc:82（`fp.srcStride = m * 100`）+ :84（`fp.unitFlag = 4`） |
| 报错模式 | EZ9999 + retCode=0x26 + aic error mask=0x6500020bd00028c + fixp_error0=0x9e + fixp_error1=0x2 + subErrType=4 |
| 内存区域 | L0C/CO1（FixPipe 读源，dav-2201 CO1 buffer = 1024 floats） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 双注入（unitFlag 4→移除 + srcStride m*100→m）+ return 1→0 |
| behavior_note | unitFlag=4 未触发 L77，srcStride=3200 OOB 兜底覆盖 L78（L0C 读越界）；变体 1/2/0xFF hang（exit 124），仅 unitFlag=4 exit=1+EZ9999 |

## 触发条件

**违规模式**：完整 Cube 流水线（Nd2Nz→LoadData2D→Mmad→Fixpipe），Fixpipe 阶段双路径注入：①`fp.unitFlag = 4`（非 0 值，n=32 非 64 对齐，主攻 L77 b4 输出 N 非 64 对齐）；②`fp.srcStride = m * 100`（srcStride=3200，OOB 兜底 L78 L0C 读越界）。无论 unitFlag 是否命中 L77，srcStride OOB 保证 EZ9999 必触发。

**安全模式**：移除 `fp.unitFlag` 赋值（保持默认 0），`fp.srcStride = m`（合法值 32）。

**变体选择结论**：unitFlag=1/2/0xFF 运行时 hang（aclrtSynchronizeStream 不返回，exit 124）；仅 unitFlag=4 满足后置条件（exit=1 + EZ9999），但 errcode 为 OOB 兜底 L78，unitFlag 未独立触发 L77。

## 检测规则

### fixp_b4_misalign-RULE-01：FixpipeParamsV220.unitFlag 非法值注入（主路径 L77）

- 【条件】`fp.unitFlag` 赋值为非 0 值（如 4），配合 n 非 64 对齐，主攻 L77 b4 输出 N 非 64 对齐
- 【API】`fp.unitFlag = 4;`（error 第 84 行）vs 移除该行（fixed）
- 【约束】unitFlag 非 0 时 b4 输出 N 对齐要求 64B 倍数；n=32 非 64 对齐 → L77（实际未命中）
- 【匹配模式】diff_pattern

### fixp_b4_misalign-RULE-02：srcStride OOB 兜底（L78 实际覆盖）

- 【条件】`fp.srcStride = m * 100`（=3200）远超 CO1 buffer 容量，单块 L0C 读越界
- 【API】`fp.srcStride = m * 100;`（error 第 82 行）vs `fp.srcStride = m;`（fixed）
- 【约束】srcStride=3200 > CO1 容量 1024 floats → L0C 读越界 = L78
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：移除 unitFlag 赋值（恢复默认 0），srcStride 恢复为合法值 m。

```cpp
// 修复前（error）
fp.srcStride = m * 100;
fp.dstStride = n;
fp.unitFlag = 4;

// 修复后（fixed）
fp.srcStride = m;
fp.dstStride = n;
// fp.unitFlag 保持默认 0（赋值行移除）
```

**防御性编程建议**：

- 禁止随意设置 unitFlag，仅按 CANN 文档允许的合法值配置
- 设置 unitFlag 前确认 n 满足对应对齐要求（64B 倍数）

## 变体索引

| 变体 | unitFlag | srcStride | 运行结果 | 采纳 |
|------|----------|-----------|---------|------|
| 1 | 1 | m*100 | hang（exit 124） | ✗ |
| 2 | 2 | m*100 | hang（exit 124） | ✗ |
| 3（基准） | 4 | m*100 | exit=1 + EZ9999（OOB 兜底 L78） | ✓ |
| 4 | 0xFF | m*100 | hang（exit 124） | ✗ |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：unitFlag 合法 | unitFlag=0（默认），srcStride 合法 | fixed 版 |
| EC2：unitFlag 非法但 hang | unitFlag=1/2/0xFF → 运行 hang | 变体 1/2/4 |
| EC3：unitFlag 注入 + OOB 兜底 | unitFlag=4 + srcStride OOB → L78 兜底 | error 版（变体 3） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| fixpipe_addr_misalign (15) | 同族 srcStride OOB 路径（L78），本用例额外注入 unitFlag |
| fixp_ch_split_illegal (L77 尝试) | 同族 fixp 非法配置探索，注入 isChannelSplit（同为 OOB 兜底 L78） |
| fixpipe_l0c_conflict (16) | 同族 L77 fixp 非法配置，dstStride=0 子条件路径 |

## 检测执行指引

- **适用匹配模式**：diff_pattern（`fixp_b4_misalign-RULE-01`）；diff_pattern（`fixp_b4_misalign-RULE-02`）。
- **命中判定**：分别记录 RULE-01 的 unitFlag 注入差异和 RULE-02 的 srcStride OOB 兜底差异；本种子的实际覆盖为 L78，不能将 unitFlag=4 的存在直接判为 L77 独立命中。保留以下参数、修复对照和运行签名核对步骤：

  1. 读取 error/main.asc 第 82 行 `fp.srcStride = m * 100`（srcStride=3200），第 84 行 `fp.unitFlag = 4`。
  2. 计算 srcStride=3200 > CO1 容量 1024 floats → L0C 读越界（OOB 兜底 L78）。
  3. unitFlag=4 未独立触发 L77（errcode 与 fixpipe_addr_misalign 一致）。
  4. fixed 版第 82 行 `fp.srcStride = m`，第 84 行 unitFlag 赋值移除 → 安全。
  5. error 版 stderr 应含 EZ9999 + retCode=0x26 + errcode:(0, 0x4000000000, 0) + fixp_error0=0x9e + subErrType=4。
- **修复映射**：
  - `fixp_b4_misalign-RULE-01` → 移除 `fp.unitFlag = 4` 赋值，恢复默认 0；保留变体 1/2/0xFF 的 hang 结论，不将其作为替代修复。
  - `fixp_b4_misalign-RULE-02` → 将 `fp.srcStride = m * 100` 恢复为 `fp.srcStride = m`，消除种子的 L78 读越界兜底。
