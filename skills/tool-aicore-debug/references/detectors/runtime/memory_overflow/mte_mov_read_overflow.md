# mte_mov_read_overflow — MTE1 MOV 读 L1 地址越界

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | mte_mov_read_overflow |
| 硬件事件编号 | L57 |
| 异常类型 | MTE1 MOV 读 L1 地址越界（GDMA_READ_OVERFLOW） |
| 错误码 | errcode:TBD（预期 MTE read 族） |
| 类别 | memory_overflow |
| 源文件 | error/main.asc:23（asc_copy_l12l0a start_index 注入） |
| 报错模式 | EZ9999 + retCode=0x26 [aicore exception] + aic error mask=0x6500020bd00028c |
| 内存区域 | L1（`__cbuf__` half*aL1，声明 M*K=256 half=512B） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | API 参数注入（start_index: 60000→0）+ return 1→0 |
| behavior_note | asc_copy_l12l0a 大 start_index=60000 读超 __cbuf__ L1 声明范围（256 half） |

## 触发条件

**违规模式**：TC-Low cube 流水线中，`__cbuf__` L1 源 `aL1` 仅声明 M*K=256 half（512B），error 版向 `asc_copy_l12l0a` 传入 start_index=60000（BAD_START_INDEX），使 MTE1 读 L1 基址远超 `aL1` 声明范围 → GDMA_READ_OVERFLOW → L57。

**安全模式**：`asc_copy_l12l0a(aL0a, aL1, 0, ...)`，start_index=0，读 L1 合法基址。

## 检测规则

### mte_mov_read_overflow-RULE-01：asc_copy_l12l0a start_index 超 L1 声明范围

- 【条件】asc_copy_l12l0a 的 start_index（src 偏移）超过 `__cbuf__` L1 源数组声明大小
- 【API】`asc_copy_l12l0a(aL0a, aL1, BAD_START_INDEX, ...)`；BAD_START_INDEX=60000
- 【约束】aL1 声明 `__cbuf__ half aL1[M*K]` = 256 half（512B）；start_index=60000 远超 256 → L1 读越界
- 【匹配模式】值域判断

### mte_mov_read_overflow-RULE-02：start_index 常量注入

- 【条件】start_index 通过 BAD_START_INDEX 常量注入，值远超 L1 声明范围
- 【API】`constexpr uint16_t BAD_START_INDEX = 60000;`
- 【约束】60000 > 256（L1 声明 half 数）→ 读地址超出 aL1 数组边界
- 【匹配模式】diff_pattern

## 修复方案

**标准修复模式**：将 start_index 设为 0，读 L1 合法基址。

```cpp
// 修复前（error）
constexpr uint16_t BAD_START_INDEX = 60000;
asc_copy_l12l0a(aL0a, aL1, BAD_START_INDEX, (uint8_t)((K + 15) / 16), (uint16_t)((M + 15) / 16), 0);

// 修复后（fixed）
asc_copy_l12l0a(aL0a, aL1, 0, (uint8_t)((K + 15) / 16), (uint16_t)((M + 15) / 16), 0);
```

**防御性编程建议**：

- 调用前断言 `start_index + repeat_count × src_stride ≤ L1_array_size`
- 对 L1 源数组声明大小做编译期常量，与 start_index 做静态比较

## 变体索引

| 变体 | start_index | L1 声明 | 越界 |
|------|------------|---------|------|
| 基准（本用例） | 60000 | 256 half | 远超 |
| 边界变体 | 256 | 256 half | 恰到边界 |
| 小偏移变体 | 257 | 256 half | +1 越界 |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：L1 读安全 | start_index ≤ L1_array_size - read_count | fixed 版（0 ≤ 256-16） |
| EC2：L1 读越界 | start_index > L1_array_size | error 版（60000 > 256） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| mte1_write_overflow (L56) | 写侧对偶：asc_copy_l12l0b dst_gap 写越界（本用例为读侧 start_index 越界） |
| mte_gdma_read_overflow (L28) | 同族 GDMA_READ_OVERFLOW，注入介质不同（本用例 TC-Low asc_copy 读 L1，L28 为 TC-High DataCopy 读 GM） |
| cube_l0c_overflow (L13) | 同骨架 cube 流水线（复用 cube_l0c_overflow 骨架） |

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01）、diff_pattern（RULE-02）。
- **命中判定**：按下列步骤核对 RULE-01 的 start_index 与 L1 源数组声明范围，并用 RULE-02 对照 BAD_START_INDEX 常量注入与修复差异。
  1. 读取 error/main.asc 第 9 行 `BAD_START_INDEX = 60000`。
  2. 确认第 15 行 `__cbuf__ half aL1[M*K]` = 256 half。
  3. 确认第 23 行 `asc_copy_l12l0a(aL0a, aL1, BAD_START_INDEX, ...)` → start_index=60000 > 256 → 越界。
  4. fixed 版 start_index=0 → 0 < 256 → 安全。
  5. error 版 stderr 应含 EZ9999 + retCode=0x26 + aic error mask=0x6500020bd00028c。
- **修复映射**：RULE-01、RULE-02 → 将实际调用的 start_index 设为 0，使用合法 L1 基址，并检查 `start_index + repeat_count × src_stride ≤ L1_array_size`。
