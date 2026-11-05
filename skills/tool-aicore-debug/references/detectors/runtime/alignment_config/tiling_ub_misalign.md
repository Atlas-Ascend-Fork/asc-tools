# tiling_ub_misalign — Tiling 驱动 UB 未对齐

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | tiling_ub_misalign |
| 异常类型 | Tiling驱动UB未对齐 |
| 错误码 | 无（EZ9999 报错无数值型错误码） |
| 类别 | alignment_config |
| 源文件 | 来源: stable_ez9999 历史种子（legacy: A2） |
| 报错模式 | EZ9999_stderr |
| 内存区域 | UB |
| kernel_diff_type | host_only_diff |
| behavior_note | kernel 函数体与修复版完全一致，差异仅在 host 侧 |

## 触发条件

Tiling 参数（从 Host 传递到 Kernel 的运行时参数）中包含导致 UB buffer 起始地址非 32B 对齐的值。与 ub_addr_misalign 的区别在于：ub_addr_misalign 是代码中硬编码的非对齐偏移，tiling_ub_misalign 是 tiling 参数运行时传入的非对齐值。历史种子（legacy: A2） 中 `tiling.misalignBytes = 2` 经 struct 传入 kernel，在 `(__ubuf__ float*)(raw + tiling.misalignBytes)` 处触发非对齐。

- 违规模式：`tiling.misalignBytes = 2`，kernel 中 `(__ubuf__ float*)(raw + tiling.misalignBytes)` 偏移 2 非 32B 对齐
- 安全模式：`tiling.misalignBytes = 0`
- 最小触发条件：TilingStruct 中 `misalignBytes` 字段不为 0 且不是 32 的倍数，经 tiling 参数传播链（Host → GM → Kernel）在 Kernel 侧驱动 UB 地址非对齐

## 检测规则

### tiling_ub_misalign-RULE-01：TilingStruct misalignBytes 字段非零非对齐

**【条件】**代码中出现 `tiling.misalignBytes = VALUE`，VALUE 为编译期常量且 `VALUE % 32 != 0`
**【API】**Tiling 参数赋值
**【约束】**`VALUE % 32 == 0`（VALUE = 0 视为安全）
**【来源】**代码 diff 直接提取
**【匹配模式】** 值域判断

匹配示例（KB）：

- `tiling.misalignBytes = 2` — 2 % 32 = 2 ≠ 0

匹配示例（detector 补充）：

```cpp
// 匹配：misalignBytes 设为非零非对齐值
tiling.misalignBytes = 2;
tiling.misalignBytes = tilingCalc();  // 动态值，需标记检查
```

### tiling_ub_misalign-RULE-02：Tiling 参数驱动的 UB 地址计算

**【条件】**代码中出现 `(__ubuf__ T*)(base + tiling->FIELD)` 或 `(__ubuf__ T*)(base + tiling.FIELD * sizeof(T))`，tiling 字段直接用于地址偏移且无 `(FIELD + 31) & ~31U` 对齐保护
**【API】**Tiling 参数 → UB 地址计算
**【约束】**`(base + tiling->FIELD) % 32 == 0`
**【来源】**代码 diff 直接提取
**【匹配模式】** API 调用模式

匹配示例（detector 补充）：

```cpp
__ubuf__ T* ptr = (__ubuf__ T*)(base + tiling->FIELD);
__ubuf__ T* ptr = (__ubuf__ T*)(base + tiling.FIELD * sizeof(T));
```

### tiling_ub_misalign-RULE-03：Host 侧 Tiling 计算未考虑对齐

**【条件】**Host 侧代码中 `tiling.misalignBytes = someCalculation()`，计算结果可能产生非 32B 对齐值
**【API】**Host 侧 Tiling 计算函数
**【约束】**计算结果 % 32 == 0
**【来源】**LLM 推断
**【匹配模式】** 变量名匹配

### tiling_ub_misalign-RULE-04：Kernel 入口缺少 tiling 参数验证

**【条件】**Kernel 入口处解引用 tiling 结构体后直接用于地址计算，未验证 `tiling->misalignBytes % 32 == 0`
**【API】**Kernel 入口参数
**【约束】**`tiling->misalignBytes % 32 == 0`
**【来源】**LLM 推断
**【匹配模式】** 存在性检查

### tiling_ub_misalign-RULE-05：尾块 tiling 参数非对齐

**【条件】**尾块处理中 `tailOffset * sizeof(T) % 32 != 0`，或 `tiling->totalElements - tiling->alignedElements` 产生的偏移非对齐
**【API】**Tiling 尾块参数
**【约束】**`tailOffset * sizeof(T) % 32 == 0`
**【来源】**LLM 推断
**【匹配模式】** 值域判断

匹配示例（detector 补充）：

```cpp
if (isTailBlock) {
    uint32_t tailOffset = tiling->totalElements - tiling->alignedElements;
}
```

## 修复方案

### 标准修复模式

1. **修复模式 1（Host 侧确保对齐）**：`tiling.misalignBytes = (rawOffset + 31) & ~31` 或直接置零
2. **修复模式 2（Kernel 侧防御性对齐）**：`uint32_t alignedOffset = (tiling->misalignBytes + 31) & ~31U`
3. **修复模式 3（修复 TilingStruct 定义）**：添加 `static_assert(tiling.misalignBytes % 32 == 0)`
4. **修复模式 4（尾块对齐）**：确保尾块偏移满足 `tailStart * sizeof(T) % 32 == 0`

### 具体修复（legacy: A2）

`tiling.misalignBytes = 2` → `tiling.misalignBytes = 0`

### 代码模板

模板 1：安全的 Tiling 参数处理（detector 补充）

```cpp
template<typename T>
class SafeTilingKernel {
public:
    __aicore__ inline void Init(GM_ADDR x, GM_ADDR y, __gm__ uint8_t* tilingData) {
        auto tiling = (__gm__ TilingParam_t*)tilingData;
        totalLength = tiling->totalLength;
        tileLength = tiling->tileLength;
        uint32_t safeMisalign = (tiling->misalignBytes + 31) & ~31U;
        xGM.Init(x, safeMisalign);
        yGM.Init(y, safeMisalign);
    }
private:
    uint32_t totalLength;
    uint32_t tileLength;
};
```

模板 2：Host 侧对齐计算（detector 补充）

```cpp
uint32_t AlignUpTo32B(uint32_t value) {
    return (value + 31) & ~31U;
}

void ComputeTiling(TilingParam_t& tiling, uint32_t totalElements, uint32_t tileLength) {
    tiling.totalLength = totalElements;
    tiling.tileLength = tileLength;
    tiling.misalignBytes = 0;  // 始终保持对齐
    uint32_t remainElements = totalElements % tileLength;
    if (remainElements > 0) {
        uint32_t tailStart = totalElements - remainElements;
        tiling.tailOffset = AlignUpTo32B(tailStart * sizeof(float)) / sizeof(float);
    }
}
```

### 防御性编程建议

1. Host 侧保证对齐，Tiling 计算函数输出值满足 32B 对齐
2. Kernel 侧防御性对齐处理
3. Tiling 计算函数单元测试覆盖对齐边界
4. TilingStruct 注释标注对齐要求
5. Host/Kernel 双重检查，确保 tiling 参数传播链上对齐约束不丢失

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| misalignBytes 非零非对齐 | stable_ez9999 历史种子（legacy: A2） | `tiling.misalignBytes = 2`，经 struct 传入 kernel | EC3 |
| tiling 偏移字段非对齐 | 变体（待生成） | `(__ubuf__ T*)(base + tiling->FIELD)` 当 FIELD % 32 != 0 | EC3 |
| 多参数组合非对齐 | 变体（待生成） | `tiling.baseOffset + tiling.extraOffset` 组合结果非对齐 | EC3 |
| 尾块偏移非对齐 | 变体（待生成） | 尾块 tiling 中 `remainSize * sizeof(T)` 非对齐 | EC3 |

## 等价类划分

变异维度：tiling.misalignBytes 值

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | misalignBytes = 0 | 0 | 无偏移，安全 |
| EC2 | misalignBytes 为 32 的正整数倍 | 32, 64, ... | 满足对齐约束，安全 |
| EC3 | misalignBytes 非 32 的整数倍 | 1-31, 33-63, ... | 违反对齐约束，触发 ADDR_MISALIGN |

划分依据：与 ub_addr_misalign 相同的 32B 对齐约束，但偏移值由 tiling 参数在运行时传入而非硬编码。

## 关联异常

- **ub_addr_misalign**：共享 ADDR_MISALIGN 根因。ub_addr_misalign 为硬编码偏移（编译期可确定），tiling_ub_misalign 为 tiling 参数驱动（需 Host/Kernel 联合分析）。
- **tiling_overflow**：共享 Tiling 参数传播链模式。tiling_ub_misalign 的 tiling.misalignBytes 经 Host → GM → Kernel 传播，tiling_overflow 的 tiling.copyBytes 同样经此传播链。
- **共享参数**：（推断，非 cross_references "参数共享"表收录）与 ub_addr_misalign 共享"UB 指针对齐约束"；与 tiling_overflow 共享 Tiling 参数 Host→Kernel 传播路径。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/05）、API 调用模式（RULE-02）、变量名匹配（RULE-03）、存在性检查（RULE-04）
- **命中判定**：
  - 值域判断类规则——在源码定位 `tiling.misalignBytes = VALUE` 赋值，VALUE 为编译期常量时计算 `VALUE % 32`；尾块偏移计算 `tailOffset * sizeof(T) % 32`
  - API 调用模式类规则——统计 `(__ubuf__ T*)(base + tiling->FIELD)` 调用序列，验证是否存在 `(FIELD + 31) & ~31U` 对齐保护
  - 变量名匹配类规则——在 Host 侧定位 `tiling.misalignBytes` 赋值表达式，追溯 `someCalculation()` 计算路径
  - 存在性检查类规则——全文模式搜索 Kernel 入口处是否存在 `tiling->misalignBytes % 32 == 0` 的验证逻辑（缺失即命中）
- **修复映射**：
  - tiling_ub_misalign-RULE-01（misalignBytes 字段）→ 修复模式 1（Host 侧确保对齐）
  - tiling_ub_misalign-RULE-02（地址计算无对齐保护）→ 修复模式 2（Kernel 侧防御性对齐）
  - tiling_ub_misalign-RULE-03（Host 侧计算未考虑对齐）→ 修复模式 1 / 修复模式 3
  - tiling_ub_misalign-RULE-04（Kernel 入口缺验证）→ 修复模式 2 / 修复模式 3
  - tiling_ub_misalign-RULE-05（尾块非对齐）→ 修复模式 4（尾块对齐）
