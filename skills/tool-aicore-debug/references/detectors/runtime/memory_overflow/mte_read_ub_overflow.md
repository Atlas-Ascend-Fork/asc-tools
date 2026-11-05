# mte_read_ub_overflow — MTE 读 UB 溢出

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | mte_read_ub_overflow |
| 异常类型 | mte_ub_rd_ovflw |
| 错误码 | 125 |
| 类别 | memory_overflow |
| 源文件 | stable_ez9999 历史种子（V2s） |
| 报错模式 | EZ9999_stderr |
| 内存区域 | UB |
| kernel_diff_type | kernel_diff |
| behavior_note | UB→GM 普通 DataCopy 的 srcStride 驱动 UB 读越界 |

## 触发条件

普通 `DataCopy(..., DataCopyParams)` 分块搬运中，srcStride 是 UB 侧前一块尾到后一块头的间隔。blockLen 与 stride 均以 32B 为单位：

`span_bytes = (blockCount * blockLen + (blockCount - 1) * srcStride) * 32`

检查 `offset_bytes + span_bytes <= allocation_bytes`。stride=0 合法；blockCount=1 时 stride 不参与跨度。先校验字段范围，再计算源、目标各自的跨度。DataCopyPad 等其他重载不能套用本式，详见 `references/constraints/semantic_contract.md`。

- 历史违规种子 V2s：`DataCopyParams params{2, 1, 65535, 0}`，UB 侧跨度 `(2*1 + 65535)*32 = 2097184 bytes`，超过种子 UB 范围。
- 历史安全种子：`DataCopyParams params{1, 8, 0, 0}`，两侧跨度均为 256 bytes；仍需各自实际分配不少于 256 bytes。
- 历史 192KiB 仅是种子阈值；当前诊断使用实际分配、起始偏移和架构边界，未知时 `runtime_required=true`。

## 检测规则

### mte_read_ub_overflow-RULE-01：UB 侧跨度超过已确认边界

**【条件】**普通 DataCopy 的 UB 侧起始偏移加实际跨度超出其可访问分配
**【API】**DataCopyParams.srcStride / blockLen / blockCount
**【约束】**字段合法，且 `offset_bytes + (blockCount*blockLen + (blockCount-1)*srcStride)*32 <= allocation_bytes`；允许 stride=0
**【来源】**代码 diff 直接提取
**【匹配模式】** 值域判断

### mte_read_ub_overflow-RULE-02：tiling 或循环偏移驱动越界

**【条件】**tiling/循环决定 count、len、gap 或 UB offset，代入 RULE-01 后已确认超限
**【API】**DataCopy 参数计算与 LocalTensor 偏移
**【约束】**每次调用均满足分配边界；不是将整个循环的操作量简单相加
**【来源】**LLM 推断
**【匹配模式】** 值域判断

### mte_read_ub_overflow-RULE-03：搬运方向区分（辅助规则）

**【条件】**存在 UB→GM DataCopy，UB 侧使用 srcStride
**【API】**DataCopy 源/目标 tensor 位置
**【约束】**区分 srcStride 读 UB、dstStride 写 UB 和 DataCopyPad 的 padding 风险；API 方向本身不是异常命中，不计异常命中率
**【来源】**LLM 推断
**【匹配模式】** API 调用模式

### mte_read_ub_overflow-RULE-SRCSTRIDE：历史超大 stride 种子模式

**【条件】**srcStride=65535 等大值与实际 blockCount/blockLen/offset 共同使跨度超出已确认分配
**【API】**DataCopy（DataCopyParams，UB→GM）
**【约束】**按 RULE-01 检查完整跨度，不能仅因 stride>=65535 就判定越界；单块不受 gap 影响
**【来源】**LLM 推断-已验证
**【匹配模式】** 值域判断

此规则与 RULE-01 指向同一越界证据时只计一次，不能因同一个注入值重复增加命中数。

## 修复方案

### 标准修复模式

1. 先核对单位、分配和布局来源；更正错误参数计算或为目标布局分配足够空间。
2. 不静默截短 blockCount、blockLen 或 stride；改变它们可能丢数据或改变矩阵布局。
3. 如果原布局需要大跨度，按等价布局重新分块搬运，并检查源、目标两侧范围。
4. 在 Host tiling 侧加入下面的检查；未知容量时停止自动替换并请求相应证据。

### seed 具体修复

历史种子从 `DataCopyParams{2, 1, 65535, 0}` 改为 `DataCopyParams{1, 8, 0, 0}`，仅表示该种子的 error/fixed 差异，不是通用修复常量。

### 代码模板

Host 侧检查普通 DataCopy 的 UB 侧跨度。输入 count/len/gap 是 DataCopyParams 字段值，offset/capacity 为字节。此检查不代替 GM 侧检查或目标架构的额外限制。

```cpp
#include <cstdint>

inline bool ValidateDataCopyFromUb(uint32_t blockCount, uint32_t blockLen,
                                   uint32_t gap, uint64_t offsetBytes,
                                   uint64_t capacityBytes) {
    if (blockCount == 0 || blockCount > 65535 ||
        blockLen == 0 || blockLen > 65535 || gap > 65535) return false;
    uint64_t spanBytes = (uint64_t(blockCount) * blockLen +
                          uint64_t(blockCount - 1) * gap) * 32ULL;
    return offsetBytes <= capacityBytes && spanBytes <= capacityBytes - offsetBytes;
}
```

### 防御性编程建议

- 分别检查两侧容量；对 UB→GM，另一侧的 dstStride 使用其独立范围。
- 覆盖零 gap、单块大 gap、刚好到边界、末块越界和偏移已越界。
- 缺少 ASSERT 只能作为防御建议，不能单独证明越界。

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| 超大 stride 种子 | stable_ez9999 历史种子 V2s | count=2,len=1,gap=65535 | EC2 |
| 紧密排列 | 参数示例 | count=2,len=1,gap=0,capacity=64 bytes | EC1 |
| 单块大 gap | 参数示例 | count=1,len=1,gap=65535,capacity=32 bytes | EC1 |
| tiling/循环偏移 | 参数传播场景 | 完整跨度超分配 | EC2/EC3 |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1 | 此侧范围合法 | 字段合法且 offset+span<=allocation |
| EC2 | 已确认越界 | 字段合法且 offset+span>allocation |
| EC3 | 待验证 | 分配、重载或参数值不能确定 |

## 关联异常

- mte_write_ub_overflow：相反方向的 UB 越界。
- datacopypad_ub_overflow：DataCopyPad 的重载和 padding 检查，单位须独立确认。
- gm_overflow：同一次 DataCopy 的 GM 侧风险，按源/目标地址分开判断。
- ub_capacity_overflow：总分配超限，不能与一次搬运的跨度越界混为一谈。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02/SRCSTRIDE）、API 调用模式（RULE-03）。
- **命中判定**：先确认普通 DataCopy 重载和 UB→GM，将所有量统一为字节，按完整跨度与实际分配比对。RULE-01 与 RULE-SRCSTRIDE 的同一证据去重；RULE-03 只作区分。跨函数无法定值时 `trace_limited=true`；分配或参数需运行验证时 `runtime_required=true`。
- **修复映射**：根因规则 → 修正参数/布局/分配并保持算子语义；辅助规则 → 区分其他候选；证据不足时只给校验方案，不照抄历史 fixed 参数。
