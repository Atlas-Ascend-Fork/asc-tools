# load2d_write_overflow — LoadData2D 写 L0A 越界

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | load2d_write_overflow |
| 硬件事件编号 | L50 |
| 异常类型 | LoadData2D 写 L0A 越界 |
| 需求目标 | AICoreError.md L50 WRITE_OVERFLOW — EventID 0x80C98002 |
| 实际覆盖 | AICoreError.md L50 WRITE_OVERFLOW — 直接触发（dstGap=128, repeatTimes=16 致 dst L0A 写跨度 2304 half >> 128 bytes） |
| 错误码 | errcode:(0x1000000000000, 0, 0) The write address of the mte load2d instruction is greater than the maximum destination address. |
| 类别 | memory_overflow |
| 源文件 | error/main.asc:33-34（`ldA.repeatTimes = 16; ldA.dstGap = 128;`） |
| 报错模式 | EZ9999 + errcode (0x1000000000000, 0, 0) |
| 内存区域 | A2/L0A（LoadData2D a1→a2 写目标，TOTAL_LENGTH×sizeof(half)=64×2=128 bytes） |
| API层级 | TC-High |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 常量注入（repeatTimes=16→1, dstGap=128→0）+ return 1→0 |
| behavior_note | LoadData2D 单队列流水线，dstGap=128×repeatTimes=16 致 dst 写跨度 2304 half=4608 bytes 远超 A2 buffer 128 bytes → L50 WRITE_OVERFLOW |

## 触发条件

**违规模式**：LoadData2D 单队列流水线（DataCopy GM→A1→LoadData2D A1→A2），通过 `ldA.dstGap=128, ldA.repeatTimes=16`（srcStride=0 使 src A1 反复读同一 32B 不越界）使 dst（A2/L0A）写跨度 16×(16+128)=2304 half=4608 bytes 远超 A2 buffer（TOTAL_LENGTH×sizeof(half)=64×2=128 bytes），触发 AICoreError.md L50 WRITE_OVERFLOW 检测。

**安全模式**：`ldA.repeatTimes = 1; ldA.dstGap = 0; ldA.srcStride = 0` — 仅 1 次 repeat，无间隔，dst 写跨度 16 half < 128 bytes。

## 检测规则

### load2d_write_overflow-RULE-01：LoadData2D dst 写跨度超 A2 buffer

- 【条件】LoadData2D 的 dst 写跨度 repeatTimes×(16+dstGap) > A2 buffer 大小
- 【API】`LoadData2DParams ldA; ldA.repeatTimes = 16; ldA.dstGap = 128; ldA.srcStride = 0; LoadData(a2, a1d, ldA);`
- 【约束】A2 buffer = TOTAL_LENGTH×sizeof(half) = 64×2 = 128 bytes；dst 写跨度 = repeatTimes×(16+dstGap)×sizeof(half) = 16×(16+128)×2 = 4608 bytes >> 128 bytes → L50 WRITE_OVERFLOW
- 【匹配模式】值域判断

### load2d_write_overflow-RULE-02：repeatTimes/dstGap 常量注入

- 【条件】repeatTimes=16、dstGap=128 使 dst 写跨度远超 A2 buffer
- 【API】`ldA.repeatTimes = 16; ldA.dstGap = 128;`
- 【约束】repeatTimes=16 + dstGap=128 → 写跨度 2304 half = 4608 bytes >> A2 128 bytes
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：将 repeatTimes 从 16 改为 1，dstGap 从 128 改为 0，使 dst 写跨度落入 A2 buffer 范围。

```cpp
// 修复前（error）
ldA.repeatTimes = 16;
ldA.dstGap = 128;
ldA.srcStride = 0;

// 修复后（fixed）
ldA.repeatTimes = 1;
ldA.dstGap = 0;
ldA.srcStride = 0;
```

**防御性编程建议**：

- LoadData2D 前 assert `repeatTimes × (16 + dstGap) × sizeof(T) <= A2_buffer_size`
- 封装 LoadData2D 参数校验 wrapper，拒绝 dst 写跨度超 buffer 的组合

## 变体索引

| 变体 | repeatTimes | dstGap | dst 写跨度 | 触发路径 |
|------|-------------|--------|-----------|---------|
| 基准（本用例） | 16 | 128 | 2304 half=4608B | L50 WRITE_OVERFLOW |
| 小越界变体 | 2 | 64 | 160 half=320B | L50 WRITE_OVERFLOW |
| 合法变体 | 1 | 0 | 16 half=32B | 安全（fixed 版） |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：dst 写跨度在 A2 buffer 内 | repeatTimes×(16+dstGap)×sizeof(T) ≤ 128 bytes | fixed 版（1×16×2=32B） |
| EC2：dst 写跨度超 A2 buffer | repeatTimes×(16+dstGap)×sizeof(T) > 128 bytes | error 版（16×144×2=4608B） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| loaddata2d_read_overflow | 同族 LoadData2D，src 读取越界路径 |
| cube_l0a_wraparound (L15) | 同族 L0A 写越界，L1→L0A 搬运路径 |

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01）、diff_pattern（RULE-02）。
- **命中判定**：按下列步骤核对 RULE-01 的 dst 写跨度是否超过 A2 buffer，并用 RULE-02 对照 repeatTimes/dstGap 注入差异；同时确认 srcStride=0 以区分读侧越界。
  1. 读取 error/main.asc 第 33-34 行 `ldA.repeatTimes = 16; ldA.dstGap = 128` 确认大跨度参数。
  2. 读取第 35 行 `ldA.srcStride = 0` 确认 src 不越界（srcStride=0）。
  3. 读取第 37 行 `LoadData(a2, a1d, ldA)` 确认 LoadData2D 调用。
  4. 计算 dst 写跨度 = 16×(16+128)×2 = 4608 bytes >> A2 buffer 128 bytes → L50 WRITE_OVERFLOW；errcode (0x1000000000000, 0, 0)。
  5. fixed 版第 33-34 行 `repeatTimes=1, dstGap=0` → dst 写跨度 16×2=32 bytes ≤ 128 bytes → 合法。
  6. error 版 stderr 应含 EZ9999 + errcode (0x1000000000000, 0, 0)；fixed 版无 EZ9999。
- **修复映射**：RULE-01 → 约束 `repeatTimes × (16 + dstGap) × sizeof(T) <= A2_buffer_size`；RULE-02 → 将 `repeatTimes=16, dstGap=128` 改为 `repeatTimes=1, dstGap=0`，保留 `srcStride=0`。
