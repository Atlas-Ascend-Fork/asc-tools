# fixp_ch_split_illegal — FixPipe ch_split 非 F32 输出（isChannelSplit 注入路径，OOB 兜底 L78）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | fixp_ch_split_illegal |
| 硬件事件编号 | L77（尝试，不可达）/ L78（OOB 兜底，实际覆盖） |
| 异常类型 | FixPipe ch_split 非 F32 输出（fixp 非法配置，isChannelSplit 路径） |
| 错误码 | errcode:(0, 0x4000000000, 0) The read address of the L0C is out of range（OOB 兜底 L78） |
| 类别 | fixpipe |
| 源文件 | error/main.asc:82（`fp.srcStride = m * 100`）+ :84（`fp.isChannelSplit = true`） |
| 报错模式 | EZ9999 + retCode=0x26 + aic error mask=0x6500020bd00028c + fixp_error0=0xa0 + fixp_error1=0x2 + subErrType=4 |
| 内存区域 | L0C/CO1（FixPipe 读源，dav-2201 CO1 buffer = 1024 floats）；cGM 输出类型 half（非 F32） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 双注入（isChannelSplit true→移除 + srcStride m*100→m）+ return 1→0 |
| behavior_note | isChannelSplit=true + half 输出未触发 L77（dav-2201 硬件静默忽略），srcStride=3200 OOB 兜底覆盖 L78；纯净性探测（移除 OOB）确认组合合法无 EZ9999 |

## 触发条件

**违规模式**：完整 Cube 流水线（Nd2Nz→LoadData2D→Mmad→Fixpipe），Fixpipe 阶段双路径注入：①`fp.isChannelSplit = true` + cGM 输出类型 half（非 F32，主攻 L77 ch_split 非法配置）；②`fp.srcStride = m * 100`（OOB 兜底 L78 L0C 读越界）。纯净性探测（变体 3/4，移除 srcStride OOB）确认 isChannelSplit=true+half 在 dav-2201 上不触发任何异常 → L77 不可达。

**安全模式**：移除 `fp.isChannelSplit` 赋值（保持默认 false），`fp.srcStride = m`（合法值 32）。

## 检测规则

### fixp_ch_split_illegal-RULE-01：isChannelSplit=true + 非 F32 输出注入（主路径 L77，不可达）

- 【条件】`fp.isChannelSplit = true` 配合 cGM 输出类型 half/bf16（非 F32），主攻 L77 ch_split 非法配置
- 【API】`GlobalTensor<half> cGM; ... fp.isChannelSplit = true;`（error 第 84 行）vs 移除该行（fixed）
- 【约束】requirement：isChannelSplit=true 仅允许 F32 输出；half 输出为非法组合（实际 dav-2201 硬件静默忽略，L77 不可达）
- 【匹配模式】diff_pattern

### fixp_ch_split_illegal-RULE-02：srcStride OOB 兜底（L78 实际覆盖）

- 【条件】`fp.srcStride = m * 100`（=3200）远超 CO1 buffer 容量，单块 L0C 读越界
- 【API】`fp.srcStride = m * 100;`（error 第 82 行）vs `fp.srcStride = m;`（fixed）
- 【约束】srcStride=3200 > CO1 容量 1024 floats → L0C 读越界 = L78
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：移除 isChannelSplit 赋值（恢复默认 false），srcStride 恢复为合法值 m。

```cpp
// 修复前（error）
GlobalTensor<half> cGM;            // 非 F32 输出（half）
fp.srcStride = m * 100;
fp.dstStride = n;
fp.isChannelSplit = true;

// 修复后（fixed）
fp.srcStride = m;
fp.dstStride = n;
// fp.isChannelSplit 保持默认 false（赋值行移除）
```

**防御性编程建议**：

- isChannelSplit=true 时确保输出类型为 F32（requirement 约束）
- 禁止随意启用 isChannelSplit，按 CANN 文档确认输出类型匹配

## 变体索引

| 变体 | isChannelSplit | 输出类型 | srcStride | 运行结果 | 采纳 |
|------|----------------|---------|-----------|---------|------|
| 1（基准） | true | half | m*100 | exit=1 + EZ9999（OOB 兜底 L78） | ✓ |
| 2 | true | bfloat16_t | m*100 | 编译失败（bf16 SIMT 不支持） | ✗ |
| 3（纯净探测） | true | half | m（移除 OOB） | exit=1 无 EZ9999（组合合法） | ✗ |
| 4（纯净探测） | true | half | m（移除 OOB）n=48 | exit=1 无 EZ9999（组合合法） | ✗ |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：isChannelSplit 合法 | isChannelSplit=false（默认），srcStride 合法 | fixed 版 |
| EC2：isChannelSplit + 非 F32（静默忽略） | isChannelSplit=true + half，无 OOB → 无 EZ9999 | 变体 3/4（纯净探测） |
| EC3：isChannelSplit 注入 + OOB 兜底 | isChannelSplit=true + half + srcStride OOB → L78 兜底 | error 版（变体 1） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| fixp_b4_misalign (L77 尝试) | 同族 fixp 非法配置探索，注入 unitFlag（同为 OOB 兜底 L78，errcode 一致） |
| fixpipe_addr_misalign (15) | 同族 srcStride OOB 路径（L78），本用例额外注入 isChannelSplit |
| fixpipe_l0c_conflict (16) | 同族 L77 fixp 非法配置，dstStride=0 子条件路径 |

## 检测执行指引

- **适用匹配模式**：diff_pattern（`fixp_ch_split_illegal-RULE-01`）；diff_pattern（`fixp_ch_split_illegal-RULE-02`）。
- **命中判定**：分别记录 RULE-01 的 isChannelSplit/输出类型配置差异和 RULE-02 的 srcStride OOB 兜底差异；本文纯净性探测已确认 dav-2201 上 isChannelSplit=true+half 未独立触发 L77，实际异常归因于 L78 兜底。保留以下参数、修复对照和运行签名核对步骤：

  1. 读取 error/main.asc 第 82 行 `fp.srcStride = m * 100`（srcStride=3200），第 84 行 `fp.isChannelSplit = true`。
  2. 确认 cGM 类型为 half（非 F32 输出）。
  3. 计算 srcStride=3200 > CO1 容量 1024 floats → L0C 读越界（OOB 兜底 L78）。
  4. isChannelSplit=true+half 未独立触发 L77（dav-2201 硬件静默忽略）。
  5. fixed 版第 82 行 `fp.srcStride = m`，第 84 行 isChannelSplit 赋值移除 → 安全。
  6. error 版 stderr 应含 EZ9999 + retCode=0x26 + errcode:(0, 0x4000000000, 0) + fixp_error0=0xa0 + subErrType=4。
- **修复映射**：
  - `fixp_ch_split_illegal-RULE-01` → 移除 `fp.isChannelSplit = true` 赋值，恢复默认 false；保留该组合在本文 dav-2201 纯净性探测中未触发 L77 的限制。
  - `fixp_ch_split_illegal-RULE-02` → 将 `fp.srcStride = m * 100` 恢复为 `fp.srcStride = m`，消除种子的 L78 读越界兜底。
