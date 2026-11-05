# mte1_write_overflow — MTE1 MOV 写 L0B 地址越界

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | mte1_write_overflow |
| 硬件事件编号 | L56 |
| 异常类型 | MTE1 MOV 写 L0A/L0B 地址越界（GDMA_WRITE_OVERFLOW） |
| 错误码 | errcode:(0x1000000000000, 0, 0)（预期，写族/卷绕侧共用值） |
| 类别 | memory_overflow |
| 源文件 | error/main.asc:25（asc_copy_l12l0b dst_gap×repeat 注入） |
| 报错模式 | EZ9999 + retCode=0x26 [aicore exception] + errcode=(0x1000000000000,0,0) |
| 内存区域 | L0B（`__cb__` half*，dav-2201 = 64KB = 32768 half） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | API 参数注入（repeat/dst_gap: BAD 常量→合法值）+ return 1→0 |
| behavior_note | asc_copy_l12l0b dst_gap=8192 × repeat=8 = 128KB ≫ L0B 64KB；与 mte_mov_read_overflow 写侧对偶 |

## 触发条件

**违规模式**：TC-Low cube 流水线中，error 版向 `asc_copy_l12l0b` 注入 `dst_gap=8192 half(16KB)/步 × repeat=8 = 128KB`。写址 = bL0b_base + repeat_idx × dst_gap：repeat0→0、repeat4→64KB(=L0B 上界)→写越界。L56 触发条件：非 load mask 指令、burst_num(repeat=8)≠0、burst_len≠0、写地址溢出——三条件全满足 → GDMA_WRITE_OVERFLOW → L56。`src_stride=0` 保证 8 次均读同一 bL1 源块，故障隔离到 L0B 写侧。

**安全模式**：`asc_copy_l12l0b(bL0b, bL1, 0, (N+15)/16, (K+15)/16, 0)`，repeat=1/dst_gap=0 单次写不越界。

## 检测规则

### mte1_write_overflow-RULE-01：asc_copy_l12l0b dst_gap×repeat 超 L0B 容量

- 【条件】asc_copy_l12l0b 的 dst_gap（half 单位）× repeat 超过 L0B 容量 32768 half（64KB）
- 【API】`asc_copy_l12l0b(bL0b, bL1, 0, BAD_REPEAT, 0, BAD_DST_GAP)`；BAD_REPEAT=8, BAD_DST_GAP=8192
- 【约束】L0B 容量 = 32768 half（dav-2201 = 64KB）；8×8192=65536 > 32768 → 写越界
- 【匹配模式】值域判断

### mte1_write_overflow-RULE-02：(repeat-1)×dst_gap 超 L0B 容量（末址模型）

- 【条件】(repeat-1)×dst_gap 超过 L0B 容量边界
- 【API】同 RULE-01
- 【约束】(8-1)×8192=57344 half=112KB > 64KB → 写越界
- 【匹配模式】值域判断

### mte1_write_overflow-RULE-03：写侧越界（L56）与卷绕（L14）区分

- 【条件】errcode=(0x1000000000000,0,0) 时，通过 errorStr / error info 区分 L56 与 L14
- 【API】asc_copy_l12l0b（MTE1 MOV 写）
- 【约束】mte error info 非零 + errorStr 含 "write address" → L56；cube error info 非零 + errorStr 含 wraparound/卷绕 → L14
- 【匹配模式】存在性检查

## 修复方案

**标准修复模式**：将 repeat/dst_gap 恢复为合法值（单次写不越界）。

```cpp
// 修复前（error）
asc_copy_l12l0b(bL0b, bL1, 0, BAD_REPEAT, 0, BAD_DST_GAP);  // repeat=8, dst_gap=8192

// 修复后（fixed）
asc_copy_l12l0b(bL0b, bL1, 0, (uint8_t)((N + 15) / 16), (uint16_t)((K + 15) / 16), 0);  // repeat=1, dst_gap=0
```

**防御性编程建议**：

- 调用前断言 `repeat * dst_gap * sizeof(half) <= L0B_SIZE_BYTES`（65536）
- 封装带边界检查的 asc_copy_l12l0b wrapper

## 变体索引

| 变体 | repeat | dst_gap | repeat×dst_gap | 越界量 |
|------|--------|---------|----------------|--------|
| 基准（本用例） | 8 | 8192 | 65536 half=128KB | +64KB |
| 小 repeat 变体 | 4 | 8192 | 32768 half=64KB | 恰到边界 |
| 读侧对偶 | — | — | — | mte_mov_read_overflow (L57) |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：L0B 写安全 | repeat×dst_gap×2 ≤ 65536 | fixed 版（1×0=0） |
| EC2：L0B 写越界 | repeat×dst_gap×2 > 65536 | error 版（8×8192=65536 half） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| mte_mov_read_overflow (L57) | 读侧对偶：asc_copy_l12l0a start_index 读超 L1（本用例为写侧 dst_gap 写超 L0B） |
| cube_l0b_wraparound (L14) | 同 L0B 写址回转机制，L14 为 cube core 路径，L56 为 MTE1 MOV 路径 |
| cube_l0a_wraparound (L15) | 同族 dst_gap×repeat 机制，L0A 路径 |

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01、RULE-02）、存在性检查（RULE-03）。
- **命中判定**：按下列步骤核对写侧参数；RULE-01 使用 `repeat × dst_gap`，RULE-02 使用末址模型 `(repeat-1) × dst_gap`（本例为 `7×8192=57344 half=112KB > 64KB`），RULE-03 使用运行时 error info 区分 L56 与 L14。
  1. 读取 error/main.asc 第 9-10 行 `BAD_REPEAT=8, BAD_DST_GAP=8192`。
  2. 确认第 25 行 `asc_copy_l12l0b(bL0b, bL1, 0, BAD_REPEAT, 0, BAD_DST_GAP)`。
  3. 计算 8×8192=65536 half=128KB > L0B 64KB → 写越界。
  4. 确认第 24 行 asc_copy_l12l0a start_index=0（正常，避 L57 读侧首误）。
  5. fixed 版 repeat=1/dst_gap=0 → 单次写安全。
  6. error 版 stderr 应含 errcode=(0x1000000000000,0,0)；通过 mte/cube error info 区分 L56 vs L14。
  - `runtime_required=true`：缺少 mte/cube error info 或 errorStr 时，可保留写越界代码证据，但不能仅凭共享 errcode 区分 L56 与 L14。
- **修复映射**：RULE-01、RULE-02 → 恢复合法 repeat/dst_gap，种子修复为 `repeat=1, dst_gap=0`；RULE-03 → mte error info 非零且 errorStr 含 write address 时采用本篇写侧修复，cube error info 非零且含 wraparound/卷绕时结合 `cube_l0b_wraparound` 核对修复。
