# mte_gdma_read_overflow — MTE2 GDMA 读地址越界

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | mte_gdma_read_overflow |
| 硬件事件编号 | L28 |
| 异常类型 | MTE2 GDMA 读地址越界（GDMA_READ_OVERFLOW） |
| 错误码 | errcode:TBD（预期 MTE read 族，比较基准 mte_mov_read_overflow 0x40000000000） |
| 类别 | memory_overflow |
| 源文件 | error/main.asc:11（`BAD_OFFSET = 0x100000`）+ main.asc:33（DataCopy src 偏移） |
| 报错模式 | EZ9999 + retCode=0x31 [vector core exception] |
| 内存区域 | GM（GlobalMemory，DataCopy 读源） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | 常量注入（BAD_OFFSET: 0x100000→0）+ return 1→0 |
| behavior_note | 8-tile VEC 双缓冲流水线；tileIdx==3 时 DataCopy src GM +0x100000=1MB 远超 2KB alloc |

## 触发条件

**违规模式**：TC-High 8-tile 双缓冲 VEC 流水线（TQue depth=2），前 3 个 tile 正常建立流水线，tileIdx==3 条件分支内 `DataCopy(xL, xGlobal[tileIdx * TILE_SIZE + BAD_OFFSET], TILE_SIZE)`，src GM 偏移 0x100000=1MB（元素）即偏移地址 ~2MB 远超 host 仅分配的 2KB（1024 half），该地址落在已分配/已映射 GM 区域之外 → MTE2 GDMA 读越界 → L28。

**安全模式**：`BAD_OFFSET = 0`，`DataCopy(xL, xGlobal[tileIdx * TILE_SIZE + 0], TILE_SIZE)`，合法偏移。

## 检测规则

### mte_gdma_read_overflow-RULE-01：DataCopy src GM 偏移超 alloc 范围

- 【条件】DataCopy 的 src GlobalTensor 偏移 + count 超过 SetGlobalBuffer 声明的 alloc 范围
- 【API】`DataCopy(xL, xGlobal[tileIdx * TILE_SIZE + BAD_OFFSET], TILE_SIZE)`；BAD_OFFSET=0x100000
- 【约束】xGlobal 声明 `SetGlobalBuffer(x, TILE_SIZE * TILE_NUM)` = 1024 half（2KB）；偏移 3×128 + 0x100000 = 远超 1024 → GM 读越界
- 【匹配模式】值域判断

### mte_gdma_read_overflow-RULE-02：流水线深层条件注入（tileIdx==3）

- 【条件】tileIdx==3 条件分支内注入 BAD_OFFSET 非 0
- 【API】`if (tileIdx == 3) { DataCopy(xL, xGlobal[tileIdx * TILE_SIZE + BAD_OFFSET], TILE_SIZE); }`
- 【约束】前 3 个 tile 正常建立流水线后，第 4 个 tile 注入异常地址
- 【匹配模式】值域判断

## 修复方案

**标准修复模式**：将 BAD_OFFSET 设为 0，保持分支结构与 DataCopy 调用对称。

```cpp
// 修复前（error）
constexpr uint32_t BAD_OFFSET = 0x100000;

// 修复后（fixed）
constexpr uint32_t BAD_OFFSET = 0;
```

**防御性编程建议**：

- DataCopy 前断言 `offset + count <= globalTensor.GetLength()`
- 封装带边界检查的 DataCopy wrapper，对 tile 偏移做范围验证

## 变体索引

| 变体 | BAD_OFFSET | 偏移量 | 触发 tile | 越界 |
|------|-----------|--------|----------|------|
| 基准（本用例） | 0x100000 | 1MB 元素 | tileIdx=3 | 远超 2KB alloc |
| 小偏移变体 | 0x100 | 256 元素 | tileIdx=3 | 超出但范围小 |
| 多 tile 变体 | 0x100000 | 1MB | tileIdx=3,5,7 | 多 tile 越界 |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1：GM 读安全 | offset + count ≤ alloc_length | fixed 版（0+128=128 ≤ 1024） |
| EC2：GM 读越界 | offset + count > alloc_length | error 版（1048576+128 >> 1024） |

## 关联异常

| 关联项 | 说明 |
|--------|------|
| mte_mov_read_overflow (L57) | 同族 GDMA_READ_OVERFLOW，注入介质不同（本用例 TC-High DataCopy GM→UB，L57 为 TC-Low asc_copy_l12l0a 读 L1） |
| scalar_gm_addr_oob (L107) | 同族 GM 越界，SU SEQ 路径 |
| vec_gm_addr_oob (L159) | 同族 GM 越界，VEC Dcache 路径 |

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01、RULE-02）。
- **命中判定**：按下列步骤核对 RULE-01 的 `tileIdx × TILE_SIZE + BAD_OFFSET + TILE_SIZE` 是否超过 alloc_length，并确认 RULE-02 的 tileIdx==3 注入条件与前三个正常 tile 路径。
  1. 读取 error/main.asc 第 11 行 `BAD_OFFSET = 0x100000`。
  2. 确认第 20 行 `xGlobal.SetGlobalBuffer(x, TILE_SIZE * TILE_NUM)` = 1024 half（2KB）。
  3. 确认第 33 行 tileIdx==3 分支 `DataCopy(xL, xGlobal[3*128 + 0x100000], 128)` → 偏移远超 1024。
  4. fixed 版 `BAD_OFFSET = 0` → 偏移 3×128=384 < 1024 → 安全。
  5. error 版 stderr 应含 EZ9999 + retCode=0x31 [vector core exception]。
- **修复映射**：RULE-01、RULE-02 → 将 `BAD_OFFSET` 设为 0，保留条件分支与 DataCopy 调用对称性，并检查各 tile 的偏移加 count 不超过 GlobalTensor 声明长度。
