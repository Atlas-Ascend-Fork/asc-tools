# ub_addr_misalign — UB 地址未 32B 对齐

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | ub_addr_misalign |
| 异常类型 | ADDR_MISALIGN |
| 错误码 | 无（EZ9999 报错无数值型错误码） |
| 类别 | alignment_config |
| 源文件 | 来源: stable_ez9999 历史种子（legacy: A1） |
| 报错模式 | EZ9999_stderr |
| 内存区域 | UB |
| kernel_diff_type | host_only_diff |
| behavior_note | kernel 函数体与修复版完全一致，差异仅在 host 侧 |

## 触发条件

通过指针偏移构造 UB（Unified Buffer）指针时，偏移量不是 32 的整数倍。NPU AI Core 的向量指令要求数据地址必须对齐到 32 字节边界，否则硬件触发地址对齐异常（`aivec error, instruction address misalign`）。历史种子（legacy: A1） 中 `(__ubuf__ float*)(raw + 2)` 的偏移 2 非 32B 对齐触发此异常。

- 违规模式：`(__ubuf__ float*)(raw + 2)` — 偏移量 2 非 32B 对齐
- 安全模式：`(__ubuf__ float*)(raw + 32)` — 偏移量 32 满足对齐
- 最小触发条件：通过指针偏移构造 UB 指针时，偏移量 OFFSET 满足 `OFFSET % 32 != 0`

## 检测规则

### ub_addr_misalign-RULE-01：指针偏移构造 UB 地址（常量偏移非对齐）

**【条件】**代码中出现 `(__ubuf__ T*)(POINTER + OFFSET)` 模式，且 OFFSET 为编译期常量，`OFFSET % 32 != 0`
**【API】**`(__ubuf__ T*)` 类型转换 / 指针算术
**【约束】**`OFFSET % 32 == 0`
**【来源】**代码 diff 直接提取
**【匹配模式】** 值域判断

匹配示例（KB）：

- `(__ubuf__ float*)(raw + 2)` — OFFSET=2，2 % 32 = 2 ≠ 0

匹配示例（detector 补充）：

```cpp
// 匹配：偏移量 2 非 32B 对齐
__ubuf__ float* p = (__ubuf__ float*)(raw + 2);
// 匹配：偏移量 10 非 32B 对齐
__ubuf__ half* q = (__ubuf__ half*)(base + 10);
```

### ub_addr_misalign-RULE-02：DataCopyPad padding 参数导致非对齐

**【条件】**DataCopyPad 调用中 `PARAMS.paddingSize != 0`，且 `blockLen * sizeof(T) + paddingSize` 不是 32 的倍数
**【API】**DataCopyPad
**【约束】**`(blockLen * sizeof(T) + paddingSize) % 32 == 0`
**【来源】**API 文档约束
**【匹配模式】** 值域判断

匹配示例（detector 补充）：

```cpp
DataCopyPad(DST, SRC, PARAMS)
// 其中 PARAMS.paddingSize != 0
```

### ub_addr_misalign-RULE-03：UB buffer 重解释导致非对齐

**【条件】**将原始字节数组强制转换为类型化指针 `(__ubuf__ T*)&raw[M]`，其中 `M % 32 != 0` 或 `M * sizeof(uint8_t) % alignof(T) != 0`
**【API】**`(__ubuf__ T*)` 类型转换
**【约束】**`M % 32 == 0`
**【来源】**API 文档约束
**【匹配模式】** 值域判断

匹配示例（detector 补充）：

```cpp
__ubuf__ uint8_t raw[N];
__ubuf__ T* ptr = (__ubuf__ T*)&raw[M];
```

### ub_addr_misalign-RULE-04：Tiling 参数驱动的 UB 地址偏移

**【条件】**TilingStruct 中用于地址计算的参数（如 `tiling.offset`）使 UB 偏移 `offset % 32 != 0`，或 `tiling.startIdx * sizeof(T) % 32 != 0`
**【API】**Tiling 参数 → UB 地址计算
**【约束】**`tiling.offset % 32 == 0` 且 `tiling.startIdx * sizeof(T) % 32 == 0`
**【来源】**LLM 推断
**【匹配模式】** 变量名匹配

### ub_addr_misalign-RULE-05：循环变量驱动的 UB 地址偏移

**【条件】**循环中 `(__ubuf__ T*)(base + i * STRIDE)` 在某些迭代中 `i * STRIDE % 32 != 0`，特别是 STRIDE 不是 32 的因数时
**【API】**指针算术
**【约束】**`STRIDE % 32 == 0`
**【来源】**LLM 推断
**【匹配模式】** 值域判断

匹配示例（detector 补充）：

```cpp
for (int i = 0; i < N; i++) {
    __ubuf__ T* ptr = (__ubuf__ T*)(base + i * STRIDE);
    // 使用 ptr
}
```

## 修复方案

### 标准修复模式

1. **修复模式 1（直接声明）**：用 `__ubuf__ T name[SIZE]` 替代指针算术，编译器自动保证 32B 对齐
2. **修复模式 2（对齐偏移量）**：当必须使用偏移时，`alignedOffset = (offset + 31) & ~31U`
3. **修复模式 3（修复 Tiling 参数）**：`tiling.misalignBytes = 0`
4. **修复模式 4（修复 DataCopyPad padding）**：调整 `blockLen` 为 32B 对齐倍数，`paddingSize = 0`

### 具体修复（legacy: A1）

违规模式 `(__ubuf__ float*)(raw + 2)` → 安全模式 `(__ubuf__ float*)(raw + 32)`（偏移 2 改为 32）

### 代码模板

模板 1：安全的 UB buffer 声明（detector 补充）

```cpp
template<typename T>
class KernelSafe {
public:
    __aicore__ inline void Init(GM_ADDR x, GM_ADDR y, uint32_t totalLength) {
        // 编译器自动保证 32B 对齐
    }
private:
    __ubuf__ T xBuf[TILE_LENGTH];
    __ubuf__ T yBuf[TILE_LENGTH];
};
```

模板 2：安全的指针偏移计算（detector 补充）

```cpp
__aicore__ inline uint32_t AlignUp32(uint32_t value) {
    return (value + 31) & ~31U;
}

__ubuf__ uint8_t raw[BUFFER_SIZE];
uint32_t safeOffset = AlignUp32(computedOffset);
__ubuf__ float* ptr = (__ubuf__ float*)(raw + safeOffset);
```

### 防御性编程建议

1. 优先使用直接声明 `__ubuf__ T name[SIZE]` 而非手动指针算术
2. 封装 `AlignUp32()` 工具函数，所有偏移计算统一调用
3. Host 侧 Tiling 参数校验，确保所有地址相关参数满足对齐
4. 编译期 `static_assert` 检查可确定的偏移
5. 每次 DataCopy/DataCopyPad 参数审查地址对齐

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| 常量偏移非对齐 | stable_ez9999 历史种子（legacy: A1） | `(__ubuf__ float*)(raw + 2)`，OFFSET=2 | EC3 |
| sizeof 乘法偏移非对齐 | 变体（待生成） | `(__ubuf__ float*)(base + sizeof(T) * N)` 当 sizeof(T)*N % 32 != 0 | EC3 |
| 动态 tiling 偏移非对齐 | 变体（待生成） | `(__ubuf__ T*)(base + tiling.offset)` 当 offset % 32 != 0 | EC3 |
| DataCopyPad padding 非对齐 | 变体（待生成） | DataCopyPad 目标 buffer 起始地址 + padding 导致非对齐 | EC3 |

补充说明：变体维度覆盖 OFFSET 取 1-31 的任意非 32 倍数值。完整变体列表见知识库内部变体记录。

## 等价类划分

变异维度：UB 指针偏移量 OFFSET

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | OFFSET = 0 | 0 | 无偏移，等价于直接声明 |
| EC2 | OFFSET 为 32 的正整数倍 | 32, 64, 96, ... | 满足 32B 对齐约束，安全 |
| EC3 | OFFSET 非 32 的整数倍 | 1-31, 33-63, ... | 违反 32B 对齐约束，触发 ADDR_MISALIGN |

划分依据：NPU AI Core UB 硬件要求所有数据访问地址对齐到 32 字节边界。

## 关联异常

- **tiling_ub_misalign**：共享 ADDR_MISALIGN 根因。ub_addr_misalign 为硬编码偏移（编译期可确定），tiling_ub_misalign 为 tiling 参数驱动（需 Host/Kernel 联合分析）。区别：ub_addr_misalign 检测时机为编译期静态分析，tiling_ub_misalign 需分析 Host 侧 tiling 计算；修复位置 ub_addr_misalign 在 Kernel 代码，tiling_ub_misalign 在 Host 侧 tiling 计算 + Kernel 防御。
- **first_window_overflow**（传播链）：LoadData3D 首窗口超限可级联导致 UB 地址非对齐（first_window_overflow → ub_addr_misalign，来源：语义推断）。
- **共享参数**：（推断，非 cross_references "参数共享"表收录）与 tiling_ub_misalign 共享"UB 指针对齐约束"参数族；与 datacopy_burst_len_illegal/datacopy_burst_num_illegal/datacopypad_ub_overflow 共享 DataCopyPad 参数族（间接，经 ub_addr_misalign-RULE-02 DataCopyPad 关联）。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02/03/05）、变量名匹配（RULE-04）
- **命中判定**：
  - 值域判断类规则——提取 `(__ubuf__ T*)(POINTER + OFFSET)` 表达式并解析 OFFSET；OFFSET 为编译期常量时计算 `OFFSET % 32`，余数 ≠ 0 即命中；跨行追溯 ≤3 层，不跨函数
  - 变量名匹配类规则——在源码定位 `tiling.offset` / `tiling.startIdx` 等变量名，读其赋值表达式并验证对齐；追溯受限时标记 `trace_limited=true`
  - DataCopyPad 相关——同时读取 blockLen 与 paddingSize，验证 `(blockLen * sizeof(T) + paddingSize) % 32 == 0`
- **修复映射**：
  - ub_addr_misalign-RULE-01（常量偏移非对齐）→ 修复模式 1（直接声明）/ 修复模式 2（对齐偏移量）
  - ub_addr_misalign-RULE-02（DataCopyPad padding）→ 修复模式 4（修复 padding）
  - ub_addr_misalign-RULE-03（重解释）→ 修复模式 1 / 修复模式 2
  - ub_addr_misalign-RULE-04（Tiling 参数驱动）→ 修复模式 3（修复 Tiling 参数）
  - ub_addr_misalign-RULE-05（循环变量驱动）→ 修复模式 2（对齐 STRIDE）
