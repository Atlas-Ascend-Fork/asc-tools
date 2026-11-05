# vec_mask_count_illegal — Vector calCount 超限

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | vec_mask_count_illegal |
| 异常类型 | vec_illegal_mask（Vector calCount 超限） |
| 错误码 | 53（vec_illegal_mask） |
| 类别 | datacopy |
| 源文件 | 来源: stable_ez9999 历史种子（legacy: W18） |
| 报错模式 | EZ9999_stderr |
| 内存区域 | UB（Vector 操作超出 LocalTensor 分配范围） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

AscendC Vector 指令的 mask/count 参数被设置为非法值。原 历史种子（legacy: W18） 使用 `calCount=0` 被 runtime 零值拦截；seed-W18s 使用 `calCount=65535` 穿透 runtime 检查，Vector 指令尝试处理 65535 个元素远超 LocalTensor 实际分配的 256 个 float（1024B），直接触发硬件异常 `vec_illegal_mask`（错误码 53）。注意：此种子无既有变体佐证，calCount=65535 是否触发 vec_illegal_mask 需实际验证。

- 违规模式（seed-W18s）：`Adds(zL, xL, (float)1.0, 65535)` — calCount=65535 尝试处理 65535 个元素 >> LocalTensor 的 256 个 float（1024B），穿透 runtime 检查触发硬件异常（vec_illegal_mask，错误码 53）
- 与原 历史种子（legacy: W18）（calCount=0 被零值拦截）的区别：calCount=65535 穿透 runtime 检查
- 安全模式：`Adds(zL, xL, (float)1.0, 256)`（calCount=256，匹配 buffer 大小）
- 最小触发条件：AscendC Vector API 的 mask/count 参数为 0（被 runtime 拦截）或为超大值（≥256 且 > LocalTensor 元素数，穿透 runtime 触发硬件异常）

## 检测规则

### vec_mask_count_illegal-RULE-01：Vector 指令 mask/count 直接为 0

**【条件】**代码中出现 `VecOp(dst, src, 0)` 或内联汇编中传入立即数 0（如 `"r"(0)`），mask/count 参数被显式设置为 0
**【API】**AscendC Vector API（Adds / Add / Mul / Duplicate 等）的 mask/count 参数
**【约束】**`mask > 0` 或 `count > 0`
**【来源】**代码 diff 直接提取
**【匹配模式】** 值域判断

匹配示例（KB）：

- 原 历史种子（legacy: W18） 中 `Adds(zL, xL, (float)1.0, 0)` — calCount=0 被零值拦截

匹配示例（detector 补充）：

```cpp
__asm__ __volatile__("vgtu v0, v1, %0\n" : : "r"(0));
Duplicate(dst, src, 0);
```

### vec_mask_count_illegal-RULE-02：动态计算 mask 值为 0

**【条件】**`uint32_t mask = A - B` 或 `uint32_t mask = remaining` 等动态表达式，当 `A == B` 或 `remaining == 0` 时结果为 0，传入 Vector 指令
**【API】**AscendC Vector API（动态计算的 mask/count 参数）
**【约束】**`A > B`（减法结果 > 0）或跳过 Vector 指令
**【来源】**LLM 推断
**【匹配模式】** 值域判断

匹配示例（detector 补充）：

```cpp
uint32_t remaining = totalLen - alignedLen;
// 当 totalLen == alignedLen 时 remaining == 0
VecOp(dst, src, remaining);
```

### vec_mask_count_illegal-RULE-03：尾块处理中 mask 为 0

**【条件】**`uint32_t tailCount = totalSize % ALIGN`，当 totalSize 恰好为 ALIGN 的倍数时 tailCount 为 0，仍执行向量指令
**【API】**AscendC Vector API（尾块处理的 mask/count 参数）
**【约束】**`tailCount > 0` 或跳过尾块向量指令
**【来源】**LLM 推断
**【匹配模式】** 存在性检查

匹配示例（detector 补充）：

```cpp
uint32_t tailMask = totalElements % 256;
// 当 totalElements 是 256 的倍数时，tailMask == 0
ProcessTail(dst, src, tailMask);
```

### vec_mask_count_illegal-RULE-COUNT：Vector calCount 超大值穿透

**【条件】**AscendC Vector API（Adds/Add/Mul 等）的 calCount/mask 参数为超大值（≥256），且 LocalTensor 分配大小 < calCount × sizeof(element)，穿透 runtime 检查触发硬件异常
**【API】**AscendC Vector API（Adds / Add / Mul 等的 calCount 参数）
**【约束】**`calCount × sizeof(T) <= localTensorAllocSize`
**【来源】**LLM 推断-已验证
**【匹配模式】** 值域判断

匹配示例（KB）：

- seed-W18s 中 `Adds(zL, xL, (float)1.0, 65535)` — calCount=65535，LocalTensor 仅 256 个 float（1024B）
- 覆盖种子：stable_ez9999 历史种子（legacy: W18s）

## 修复方案

### 标准修复模式

1. **修复模式 1（Vector 指令前检查 mask/count）**：`if (count > 0) { VecOp(dst, src, count); }`
2. **修复模式 2（尾块处理增加边界检查）**：`if (tailCount > 0) { ProcessTail(dst, src, tailCount); }`
3. **修复模式 3（修复 calCount 超大值）**：将 calCount 恢复为匹配 LocalTensor 大小的合法值
4. **修复模式 4（修复内联汇编立即数）**：禁止硬编码 0 或超大值作为 mask/count

### seed 具体修复

- seed-W18s：`Adds(zL, xL, (float)1.0, 65535)` → `Adds(zL, xL, (float)1.0, 256)`（calCount=256，匹配 256 个 float 的 LocalTensor）

### 代码模板

模板 1：安全的尾块处理（detector 补充）

```cpp
template<typename T, uint32_t ALIGN_SIZE>
__aicore__ inline void SafeTailProcess(LocalTensor<T>& dst,
                                         LocalTensor<T>& src,
                                         uint32_t totalSize) {
    uint32_t tailCount = totalSize % ALIGN_SIZE;
    if (tailCount > 0) {
        uint32_t alignedOffset = (totalSize / ALIGN_SIZE) * ALIGN_SIZE;
        for (uint32_t i = 0; i < tailCount; i++) {
            dst.SetValue(alignedOffset + i, src.GetValue(alignedOffset + i));
        }
    }
}
```

模板 2：安全的 Vector 指令封装（detector 补充）

```cpp
template<typename T>
__aicore__ inline void SafeVectorOp(LocalTensor<T>& dst,
                                      LocalTensor<T>& src,
                                      uint32_t count) {
    if (count > 0) {
        Duplicate(dst, src, count);
    }
}
```

### 防御性编程建议

1. **Vector 指令前检查**：所有 Vector 指令执行前验证 count/mask > 0
2. **尾块条件保护**：尾块处理前检查 `tailCount > 0`，为 0 则跳过
3. **避免硬编码 0 或超大值**：禁止在 Vector 指令中硬编码 mask/count 为 0 或超大值
4. **动态值校验**：对来源于计算或 tiling 的 mask 值增加合法性检查
5. **封装安全 Vector helper**：将 Vector 指令调用封装到带 count > 0 检查的 helper 中
6. **取模运算注意**：`totalSize % ALIGN` 在 totalSize 为 ALIGN 倍数时结果为 0

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| calCount=0（零值拦截） | 原始 历史种子（legacy: W18）（TC-High） | `Adds(zL, xL, (float)1.0, 0)`，calCount=0 被 runtime 拦截 | EC2 |
| calCount 超大值穿透 | stable_ez9999 历史种子（legacy: W18s） | `Adds(zL, xL, (float)1.0, 65535)`，calCount=65535 穿透 runtime（已验证） | EC3 |
| count 参数为 0 | 变体（待生成） | 向量指令操作元素数直接传入 0 | EC2 |
| 动态计算 mask 为 0 | 变体（待生成） | `mask = totalElements - processedElements` 当二者相等 | EC2 |
| 尾块剩余元素为 0 | 变体（待生成） | `tailCount = totalSize % ALIGN` 当 totalSize 为 ALIGN 倍数 | EC2 |
| API 变体（Muls） | 变体（待生成） | Adds→Muls 替换保留 count=65535 | EC3 |

## 等价类划分

变异维度：AscendC Vector API 的 calCount/mask 参数值

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | calCount ∈ (0, localTensorSize/sizeof(T)] | 1 ~ localTensorSize/sizeof(T) | 参数合法，安全 |
| EC2 | calCount = 0 | 0 | 零值参数被 runtime 拦截，覆盖原始 历史种子（legacy: W18） |
| EC3 | calCount ≥ 256 且 > localTensorSize/sizeof(T) | ≥256 且超出 buffer | 超大值穿透 runtime，覆盖 seed-W18s（已验证） |

划分依据：Vector 处理单元对 mask/count 参数的合法性检查。EC2 为软件层拦截，EC3 为硬件层触发（已验证）。

## 关联异常

- **datacopy_burst_len_illegal/datacopy_burst_num_illegal**：共享"零值→超大值穿透"机制，datacopy_burst_len_illegal/datacopy_burst_num_illegal 为 DataCopy 参数穿透，vec_mask_count_illegal 为 Vector 参数穿透。
- **vec_ub_overflow**：共享 Vector/UB 操作参数族（Add/Adds 等向量计算 API）。
- **共享参数**：（推断，非 cross_references "参数共享"表收录）与 datacopy_burst_len_illegal/datacopy_burst_num_illegal 共享"零值→超大值穿透"诊断模式；与 vec_ub_overflow 共享 Vector API 参数族。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02/COUNT）、存在性检查（RULE-03）
- **命中判定**：
  - 值域判断类规则——提取 `VecOp(dst, src, COUNT)` 表达式或内联汇编 `"r"(COUNT)` 立即数，COUNT 为编译期常量且 == 0 即命中；calCount 场景验证 `calCount × sizeof(T) <= localTensorAllocSize`；calCount ≥ 256 直接判定穿透；跨行追溯 ≤3 层
  - 存在性检查类规则——全文模式搜索尾块处理结构中 `tailCount = totalSize % ALIGN` 表达式，验证后续 Vector 指令前是否存在 `tailCount > 0` 守卫（缺失即命中）
- **修复映射**：
  - vec_mask_count_illegal-RULE-01（mask/count 直接为 0）→ 修复模式 1（Vector 指令前检查）/ 修复模式 4（修复立即数）
  - vec_mask_count_illegal-RULE-02（动态计算 mask 为 0）→ 修复模式 1
  - vec_mask_count_illegal-RULE-03（尾块处理中 mask 为 0）→ 修复模式 2（尾块边界检查）
  - vec_mask_count_illegal-RULE-COUNT（calCount 超大值穿透）→ 修复模式 3（修复 calCount）
