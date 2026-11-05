# hset_counter_overflow — CUBE HSET counter 上溢出

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | hset_counter_overflow |
| 硬件事件编号 | L8 |
| 异常类型 | CUBE HSET counter 上溢出 |
| 需求目标 | AICoreError.md L8 HSET counter 上溢 |
| 实际覆盖 | ffts_cross_core_sync arrive 循环 32 次不配对 wait 探索 L8（主路径）；OOB 兜底 asc_copy_l12l0a dst_gap×repeat 致 L0A 卷绕（L15，cube_l0a_wraparound 已验证） |
| 错误码 | errcode:TBD（L8 主路径无先例，OOB 降级 L15 族） |
| 类别 | sync_hang |
| 源文件 | error/main.asc:10（`HSET_ARRIVE_COUNT_ERR = 32`）+ :19-23（ffts_cross_core_sync 循环）+ :34（asc_copy_l12l0a OOB 兜底） |
| 报错模式 | EZ9999（验证阶段补充 retCode/errcode/subErrType/cube error info 字段） |
| 内存区域 | HSET counter（ffts_cross_core_sync arrive 不配对 wait 累积）+ L0A（OOB 兜底写越界） |
| API层级 | TC-Low |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 结构变更（arrive 循环 32 次+OOB 兜底→arrive 1 次+wait 配对+合法单次写）+ return 1→0 |
| behavior_note | 主路径 32 次 ffts_cross_core_sync arrive 不配对 wait 致 HSET counter 累积上溢（L8 探索）；OOB 兜底 8×8192=65536 half=128KB >> L0A 64KB 致 L0A 卷绕（L15） |

## 触发条件

**违规模式（主路径）**：`asc_sync_block_arrive` 在 dav-2201 因 impl 内 `ffts_cross_core_sync(pipe, config)` 拒绝 runtime `pipe_t` 变量而编译失败，改用同语义 `ffts_cross_core_sync(PIPE_MTE3, config)` 直接下发 32 次 arrive（mode=0/flagID=0，config 编码与 `asc_sync_block_arrive_impl.h:25` 一致），不配对 `asc_sync_block_wait`，counter 累积致 HSET 上溢（L8 探索）。

**违规模式（OOB 兜底）**：`asc_copy_l12l0a(aL0a, aL1, 0, 8, 0, 8192)` — 8×8192=65536 half=128KB >> L0A 64KB → L0A 卷绕（cube_l0a_wraparound 已验证必触发 EZ9999）。主路径与 OOB 兜底顺序执行，任一触发即 EZ9999。

**安全模式**：`HSET_ARRIVE_COUNT_ERR = 1`（arrive 仅 1 次）+ 紧接 `asc_sync_block_wait(0)`（arrive/wait 配对，counter 不累积）；`asc_copy_l12l0a(aL0a, aL1, 0, 1, 1, 0)`（合法单次写）。

## 检测规则

### hset_counter_overflow-RULE-01：ffts_cross_core_sync arrive 循环不配对 wait

- 【条件】ffts_cross_core_sync arrive 调用次数（HSET_ARRIVE_COUNT_ERR=32）远超 counter 上限，且不配对 asc_sync_block_wait
- 【API】`for (int i = 0; i < HSET_ARRIVE_COUNT_ERR; i++) { uint64_t config = 0x1ULL | ...; ffts_cross_core_sync(PIPE_MTE3, config); }`
- 【约束】HSET arrive counter 有上限；32 次 arrive 不配对 wait → counter 累积上溢 → L8
- 【匹配模式】API 调用模式

### hset_counter_overflow-RULE-02：asc_copy_l12l0a dst_gap×repeat 致 L0A 卷绕（OOB 兜底）

- 【条件】asc_copy_l12l0a 的 repeat×dst_gap 致 L0A 写跨度超 64KB
- 【API】`asc_copy_l12l0a(aL0a, aL1, 0, L0A_REPEAT_ERR, L0A_SRC_STRIDE, L0A_DST_GAP);`（L0A_REPEAT_ERR=8, L0A_DST_GAP=8192）
- 【约束】L0A 容量 = 64KB = 32768 half；8×8192=65536 half=128KB >> 32768 half → L0A 卷绕 → L15
- 【匹配模式】值域判断

### hset_counter_overflow-RULE-03：arrive/wait 配对 + 常量注入

- 【条件】HSET_ARRIVE_COUNT_ERR 从 32 改为 1，并添加 asc_sync_block_wait(0) 配对
- 【API】`constexpr uint32_t HSET_ARRIVE_COUNT_ERR = 32;`（error）vs `constexpr uint32_t HSET_ARRIVE_COUNT_ERR = 1;`（fixed）
- 【约束】error 版 32 次 arrive 无 wait → counter 累积；fixed 版 1 次 arrive + wait 配对 → counter 不累积
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：arrive 循环次数从 32 降为 1 并添加 wait 配对；OOB 兜底改为合法单次写。

```cpp
// 修复前（error）
constexpr uint32_t HSET_ARRIVE_COUNT_ERR = 32;
// ...
#pragma unroll
for (int i = 0; i < HSET_ARRIVE_COUNT_ERR; i++) {
    uint64_t config = 0x1ULL | (static_cast<uint64_t>(0) << 4) | (static_cast<uint64_t>(0) << 8);
    ffts_cross_core_sync(PIPE_MTE3, config);
}
// ...
asc_copy_l12l0a(aL0a, aL1, 0, L0A_REPEAT_ERR, L0A_SRC_STRIDE, L0A_DST_GAP);

// 修复后（fixed）
constexpr uint32_t HSET_ARRIVE_COUNT_ERR = 1;
// ...
for (int i = 0; i < HSET_ARRIVE_COUNT_ERR; i++) {
    uint64_t config = 0x1ULL | (static_cast<uint64_t>(0) << 4) | (static_cast<uint64_t>(0) << 8);
    ffts_cross_core_sync(PIPE_MTE3, config);
}
asc_sync_block_wait(0);
// ...
asc_copy_l12l0a(aL0a, aL1, 0, 1, 1, 0);
```

**防御性编程建议**：

- 每次 ffts_cross_core_sync arrive 后必须配对 asc_sync_block_wait
- arrive/wait 配对使用，避免 counter 累积超过上限
- asc_copy_l12l0a 前 assert `repeat × dst_gap ≤ L0A_SIZE`

## 变体索引

| 变体 | arrive 次数 | wait 配对 | OOB 兜底 | 触发路径 |
|------|------------|----------|---------|---------|
| 基准（本用例） | 32 | 无 | 8×8192 | L8 + L15 |
| 仅主路径 | 32 | 无 | 无 | L8 |
| 仅 OOB | 0 | — | 8×8192 | L15 |
| 合法变体 | 1 | 有 | 1×1 | 安全（fixed 版） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：arrive/wait 配对 | arrive 次数 ≤ counter 上限 且配对 wait | fixed 版（1 次+wait） |
| EC2：arrive 不配对致 counter 上溢 | arrive 次数 >> counter 上限 且无 wait | error 版主路径（32 次无 wait） |
| EC3：L0A 写卷绕（OOB 兜底） | repeat×dst_gap > L0A_SIZE | error 版兜底（8×8192） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| cube_l0a_wraparound (L15) | OOB 兜底路径已验证用例，L0A 卷绕 |
| flag_counter_overflow (L101) | 同族 counter 溢出探索，SetFlag counter 路径 |
| sync_flag_mismatch_hang | 同族同步异常，flag 不匹配路径 |

## 检测执行指引

- **适用匹配模式**：API 调用模式（RULE-01）、值域判断（RULE-02）、diff_pattern（RULE-03）。
- **命中判定**：按下列步骤核对 RULE-01 的 arrive/wait 不配对、RULE-02 的 L0A 写越界兜底和 RULE-03 的常量及配对差异；主路径与 OOB 兜底分别记录证据。
  1. 读取 error/main.asc 第 10 行 `HSET_ARRIVE_COUNT_ERR = 32` 确认大循环次数。
  2. 读取第 19-23 行 `#pragma unroll` + `for` 循环 32 次 `ffts_cross_core_sync(PIPE_MTE3, config)` 确认 arrive 不配对 wait。
  3. 读取第 34 行 `asc_copy_l12l0a(aL0a, aL1, 0, 8, 0, 8192)` 确认 OOB 兜底（8×8192=65536 half >> L0A 32768 half）。
  4. fixed 版第 10 行 `HSET_ARRIVE_COUNT_ERR = 1` + 第 23a 行 `asc_sync_block_wait(0)` + 第 34 行 `asc_copy_l12l0a(aL0a, aL1, 0, 1, 1, 0)` → 合法。
  5. error 版 stderr 应含 EZ9999（验证阶段补充 retCode/errcode/subErrType/cube error info）。
  - `runtime_required=true`：L8 主路径需 retCode/errcode/subErrType/cube error info 确认，OOB 兜底导致的 EZ9999 不能单独确认 HSET counter 溢出。
- **修复映射**：RULE-01、RULE-03 → 将 arrive 次数从 32 降为 1 并添加 `asc_sync_block_wait(0)`；RULE-02 → 将兜底搬运恢复为 `asc_copy_l12l0a(aL0a, aL1, 0, 1, 1, 0)`，使写入落在合法范围内。
