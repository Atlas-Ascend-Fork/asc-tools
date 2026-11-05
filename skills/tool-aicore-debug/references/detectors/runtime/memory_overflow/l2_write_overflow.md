# l2_write_overflow — L2 写越界

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | l2_write_overflow |
| 异常类型 | biu_l2_write_oob |
| 错误码 | 1 |
| 类别 | memory_overflow |
| 源文件 | 来源: stable_ez9999 历史种子（legacy: T1） |
| 报错模式 | EZ9999_stderr |
| 内存区域 | L2 |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

当写入 L2 缓存的地址偏移量超出 L2 有效地址范围时，BIU（Bus Interface Unit）检测到越界写入并上报 l2_write_overflow 异常（biu_l2_write_oob）。超大偏移通常源于指针算术溢出或错误的地址计算。历史种子（legacy: T1） 中 违规模式通过 `baseAddr + 0x100000000ULL`（4GB 偏移）构造越界 GlobalTensor，使 DataCopy 写入超出 L2 有效地址范围。

- 违规模式（历史种子（legacy: T1））：`badGM.SetGlobalBuffer(reinterpret_cast<__gm__ float*>(reinterpret_cast<uintptr_t>(baseAddr) + 0x100000000ULL), TOTAL_LENGTH)` — 超大偏移（4GB）使 L2 写地址越界，触发 biu_l2_write_oob（错误码 1）
- 安全模式：`badGM.SetGlobalBuffer(reinterpret_cast<__gm__ float*>(reinterpret_cast<uintptr_t>(baseAddr)), TOTAL_LENGTH)` — 无偏移，正常地址
- 最小触发条件：L2 写操作的地址偏移超出 L2 有效地址范围

## 检测规则

### l2_write_overflow-RULE-01：超大偏移常量检测

**【条件】**L2/GM 指针加上超大常量偏移（如 `baseAddr + 0x100000000ULL`、`l2Buf + 0xFFFFFFFF`），偏移值超出 L2/GM 容量
**【API】**GlobalTensor / L2 指针算术（`+` 超大常量偏移）
**【约束】**`offset < L2_SIZE`
**【来源】**代码 diff 直接提取
**【匹配模式】** 值域判断

匹配示例（KB）：

- 历史种子（legacy: T1） 中 `baseAddr + 0x100000000ULL`（4GB 偏移）

匹配示例（detector 补充）：

```cpp
float* data = (float*)(l2Buf + 0xFFFFFFFF);
float* ptr = l2Buf + 999999;
char* dst = (char*)l2Buf + 0x7FFFFFFF;
```

### l2_write_overflow-RULE-02：指针算术溢出检测

**【条件】**L2/GM 偏移通过乘法计算（如 `rows * cols * sizeof(float)`、`blockIdx * blockDim * ELEMENTS_PER_BLOCK`），且乘法可能产生大数值导致溢出
**【API】**GlobalTensor / L2 指针算术（乘法计算的偏移）
**【约束】**`A × B` 使用 64 位整数避免溢出且 `A × B < L2_SIZE`
**【来源】**API 文档约束
**【匹配模式】** 值域判断

匹配示例（detector 补充）：

```cpp
uint64_t offset = rows * cols * sizeof(float);
float* dst = l2Buf + offset;
int64_t idx = blockIdx * blockDim * ELEMENTS_PER_BLOCK;
```

### l2_write_overflow-RULE-03：DMA 目标地址越界检测

**【条件】**DataCopy 或类似 DMA 操作的目标地址为带偏移的 L2/GM 指针，`目标地址 + 传输长度`超出 L2/GM 范围
**【API】**DataCopy（目标地址为带偏移的 L2/GM 指针）
**【约束】**`offset + write_length <= L2_SIZE`
**【来源】**LLM 推断
**【匹配模式】** API 调用模式

匹配示例（KB）：

- 历史种子（legacy: T1） 中 `DataCopy(badGM, xLocal, TOTAL_LENGTH)` — badGM 为 baseAddr+0x100000000ULL

### l2_write_overflow-RULE-04：tiling 驱动的 L2 偏移动态检测

**【条件】**tiling 参数计算得到 L2 偏移时，需在运行时验证偏移合法性，缺少 `ASSERT(offset + writeLength <= L2_SIZE)` 校验
**【API】**tiling 参数驱动的 L2 偏移
**【约束】**`offset >= 0 && offset < L2_SIZE` 且 `offset + write_length <= L2_SIZE`
**【来源】**LLM 推断
**【匹配模式】** 存在性检查

## 修复方案

### 标准修复模式

1. **修复模式 1（L2 偏移边界校验）**：`if (offset + 64 <= l2Size) { l2Buf[offset + i] = srcGlobal[i]; }`
2. **修复模式 2（使用 64 位偏移避免溢出）**：`uint64_t offset = static_cast<uint64_t>(rows) * cols * blockIdx; if (offset + copyLen <= l2Size) { DataCopy(l2Buf + offset, srcGlobal, copyLen); }`
3. **修复模式 3（DMA 目标地址合法性检查）**：`uint64_t dstOffset = static_cast<uint64_t>(computedOffset) * sizeof(float); if (dstOffset + copyBytes <= l2Size) { DataCopy(l2Buf + computedOffset, srcGlobal, totalLen); }`
4. **修复模式 4（分块写入避免超大偏移）**：按 `maxPerTile = l2Size / 2` 分块写入，避免单次超大偏移

### seed 具体修复

- 历史种子（legacy: T1）：`baseAddr + 0x100000000ULL` → `baseAddr`（即移除 4GB 超大偏移，使用正常基地址）

### 代码模板

模板 1：安全的 L2 写入辅助函数（detector 补充）

```cpp
__aicore__ inline void SafeL2Write(
    float* l2Buf, uint64_t l2SizeFloats,
    uint64_t offset, uint64_t length,
    const __gm__ float* src) {
    if (offset + length <= l2SizeFloats) {
        DataCopy(l2Buf + offset, src, length);
    }
}
```

### 防御性编程建议

1. **所有 L2 偏移使用 64 位整数**：避免 32 位乘法溢出导致错误偏移
2. **写入前验证范围**：确保 `offset + writeLength <= L2_SIZE`
3. **避免硬编码偏移**：不使用 `0xFFFFFFFF` 或 `0x100000000ULL` 等超大常量作为 L2 偏移
4. **封装 L2 访问操作**：通过辅助函数统一处理边界检查
5. **tiling 参数约束 L2 偏移**：tiling 计算应考虑 L2 容量，确保偏移合法
6. **添加 ASSERT 保护**：在 DMA 操作前添加偏移范围断言，便于调试期发现问题

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| 历史种子（legacy: T1）（直接迁移） | stable_ez9999 历史种子（legacy: T1） | `baseAddr + 0x100000000ULL` 使 L2 写地址越界触发 biu_l2_write_oob | EC2 |
| 超大偏移（常量） | 变体（待生成） | `l2Buf + 0xFFFFFFFF` 形式地址计算 | EC2 |
| L2 指针算术越界 | 变体（待生成） | `offset = largeA * largeB` 乘法溢出 | EC2 |
| DMA 搬运到 L2 越界 | 变体（待生成） | `DataCopy(l2Buf + hugeOffset, src, len)` | EC2 |
| tiling 驱动 L2 偏移 | 变体（待生成） | tiling 偏移未做边界校验 | EC2 |

## 等价类划分

变异维度：L2 写偏移相对于 L2 有效地址范围

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | offset ∈ [0, L2_SIZE) 且 offset + write_length ≤ L2_SIZE | 合法范围 | 写入地址在 L2 有效范围内，安全 |
| EC2 | offset 为编译期超大常量 | 如 0x100000000ULL（4GB） | 编译期可确定越界，覆盖 历史种子（legacy: T1） |

划分依据：BIU 对 L2 写操作地址的范围检查。

## 关联异常

- **gm_overflow**：共享"地址越界"症状模式（GM/L2 overflow 组），gm_overflow 为 GM 地址越界（读/写），l2_write_overflow 为 L2 地址越界（写）。
- **共享参数**：（推断，非 cross_references "参数共享"表收录）与 gm_overflow 共享 GM/L2 指针算术 + 超大常量偏移模式；与 gm_overflow 共享 DataCopy 目标地址越界场景。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02）、API 调用模式（RULE-03）、存在性检查（RULE-04）
- **命中判定**：
  - 值域判断类规则——提取 L2/GM 指针算术表达式（`baseAddr + OFFSET`、`l2Buf + hugeConst`），OFFSET 为编译期常量且 > L2_SIZE 即命中；乘法表达式（`rows * cols * sizeof(float)`）计算结果与 L2_SIZE 比对；跨行追溯 ≤3 层
  - API 调用模式类规则——统计 DataCopy 调用，验证目标地址是否含偏移表达式，验证 `offset + write_length <= L2_SIZE`
  - 存在性检查类规则——全文模式搜索 tiling 驱动的 L2 偏移计算，验证是否存在 `ASSERT(offset + writeLength <= L2_SIZE)` 守卫（缺失即命中）
- **修复映射**：
  - l2_write_overflow-RULE-01（超大偏移常量）→ 修复模式 1（边界校验）/ 修复模式 4（分块写入）
  - l2_write_overflow-RULE-02（乘法溢出）→ 修复模式 2（64 位偏移）
  - l2_write_overflow-RULE-03（DMA 目标越界）→ 修复模式 3（DMA 目标合法性检查）
  - l2_write_overflow-RULE-04（tiling 驱动）→ 修复模式 1 / 修复模式 2
