# fixpipe_l1_overflow — FixPipe 读 L1 越界

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | fixpipe_l1_overflow |
| 异常类型 | fixp_err_read_l1_ovflw |
| 错误码 | 103 |
| 类别 | fixpipe |
| 源文件 | 来源: stable_ez9999 历史种子（legacy: W5） |
| 报错模式 | EZ9999_stderr |
| 内存区域 | L1（通过 L0C→L1 FixPipe 读取） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

在 FixPipe 操作中，`srcStride` 被设置为过大的值，导致从 L0C 缓冲区读取数据时地址越界。硬件检测到读操作超出 L1 分配范围，触发 fixp_err_read_l1_ovflw 异常（错误码 103）。seed-W5a 中 `fp.srcStride = m * 100`（m=32, srcStride=3200）使 FixPipe 读取远超 CO1 buffer 实际布局。seed-W5b 使用相同参数值，但错误注入在 3 层嵌套 `Process → RunPipeline → ExecuteStage` 的 stageId==2 分支中，参数传播深度不同。

- 违规模式（seed-W5a 扁平）：`fp.srcStride = m * 100`（m=32, srcStride=3200）— 直接在 Process() 中设置，FixPipe 从 L0C 读取时 srcStride=3200 远超实际矩阵布局（m=32），触发 fixp_err_read_l1_ovflw
- 违规模式（seed-W5b 深度嵌套）：`fp.srcStride = m * 100`（m=32, srcStride=3200）— 注入在 ExecuteStage(stageId==2) 分支中，经 Process→RunPipeline→ExecuteStage 3 层传播
- 安全模式：`fp.srcStride = m`（即 32）— 匹配 L0C 中数据的实际存储 stride
- 最小触发条件：FixPipe 的 srcStride 参数过大，导致从 L0C 读取时地址超出 L1 分配范围

## 检测规则

### fixpipe_l1_overflow-RULE-01：srcStride 乘法计算过大

**【条件】**代码中出现 `fp.srcStride = EXPR * CONSTANT`（如 `fp.srcStride = m * 200`、`fp.srcStride = m * 100`），srcStride 由乘法计算且无 `min` 或 `ASSERT` 保护
**【API】**FixPipe（`srcStride` 参数）
**【约束】**`srcStride <= l1BufferSize`
**【来源】**代码 diff 直接提取
**【匹配模式】** 值域判断

匹配示例（KB）：

- seed-W5a/W5b 中 `fp.srcStride = m * 100`（m=32, srcStride=3200）

匹配示例（detector 补充）：

```cpp
fp.srcStride = m * 200;
fp.srcStride = cols * sizeof(float) * 2;
```

### fixpipe_l1_overflow-RULE-02：srcStride × ndNum 越界风险

**【条件】**`srcStride` 和 `ndNum` 同时设置但未校验乘积，`srcStride × ndNum` 可能超出 L1 buffer 实际大小
**【API】**FixPipe（`srcStride` × `ndNum` 组合）
**【约束】**`static_cast<uint64_t>(srcStride) × ndNum <= l1BufferSize`
**【来源】**API 文档约束
**【匹配模式】** 值域判断

匹配示例（detector 补充）：

```cpp
fp.srcStride = tileM * sizeof(float);
fp.ndNum = totalIterations;
```

### fixpipe_l1_overflow-RULE-03：多次 FixPipe 累积偏移

**【条件】**循环中多次 FixPipe 调用，`offset += fp.srcStride` 或 `srcAddr += fp.srcStride` 累积偏移可能越界
**【API】**FixPipe（循环中的累积偏移）
**【约束】**`Σ(srcStride_i × ndNum_i) <= l1BufferSize`
**【来源】**LLM 推断
**【匹配模式】** 变量名匹配

匹配示例（detector 补充）：

```cpp
srcOffset += fp.srcStride;
fp.srcAddr = baseAddr + srcOffset;
```

### fixpipe_l1_overflow-RULE-04：L1 读取范围校验缺失

**【条件】**FixPipe 调用前缺少 `srcStride × ndNum <= l1Size` 的断言或判断
**【API】**FixPipe（调用前的范围校验）
**【约束】**FixPipe 调用前存在 `srcStride × ndNum <= l1Size` 断言
**【来源】**LLM 推断
**【匹配模式】** 存在性检查

### fixpipe_l1_overflow-RULE-05：累积偏移边界检测

**【条件】**循环中多次 FixPipe 调用的累积偏移无上限保护
**【API】**FixPipe（循环中累积偏移的上限保护）
**【约束】**循环中累积偏移存在 `offset < L1_SIZE` 守卫
**【来源】**LLM 推断
**【匹配模式】** 存在性检查

## 修复方案

### 标准修复模式

1. **修复模式 1（srcStride 边界限制）**：`uint32_t maxStride = L1_SIZE / std::max(fp.ndNum, 1u); fp.srcStride = std::min(rawStride, maxStride); ASSERT(static_cast<uint64_t>(fp.srcStride) * fp.ndNum <= L1_SIZE);`
2. **修复模式 2（tiling 驱动的安全 srcStride）**：`uint32_t maxSrc = L1_SIZE / std::max(fp.ndNum, 1u); fp.srcStride = std::min(tiling.srcStep, maxSrc);`
3. **修复模式 3（多次 FixPipe 累积偏移保护）**：循环中添加 `if (offset + fp.srcStride > L1_SIZE) break;` 守卫条件
4. **修复模式 4（L0C stride 对齐）**：`uint32_t alignedStride = ((m * sizeof(float) + L0C_ALIGN - 1) / L0C_ALIGN) * L0C_ALIGN; fp.srcStride = std::min(alignedStride, maxStride);`

### seed 具体修复

- seed-W5a：`fp.srcStride = m * 100`（即 3200） → `fp.srcStride = m`（即 32）
- seed-W5b：`fp.srcStride = m * 100`（即 3200） → `fp.srcStride = m`（即 32，注入层 ExecuteStage 不变）

### 代码模板

模板 1：安全 FixPipe srcStride 计算（detector 补充）

```cpp
inline uint32_t SafeSrcStride(uint32_t rawStride, uint32_t ndNum, uint32_t l1Size) {
    uint32_t maxStride = ndNum > 0 ? l1Size / ndNum : l1Size;
    uint32_t safe = std::min(rawStride, maxStride);
    ASSERT(static_cast<uint64_t>(safe) * ndNum <= l1Size);
    return safe;
}
```

模板 2：累积偏移跟踪器（detector 补充）

```cpp
class FixPipeOffsetTracker {
    uint32_t totalOffset_ = 0;
    uint32_t l1Limit_;
public:
    explicit FixPipeOffsetTracker(uint32_t l1Limit) : l1Limit_(l1Limit) {}
    bool CanAdvance(uint32_t stride) const { return totalOffset_ + stride <= l1Limit_; }
    void Advance(uint32_t stride) { ASSERT(CanAdvance(stride)); totalOffset_ += stride; }
    uint32_t Offset() const { return totalOffset_; }
};
```

### 防御性编程建议

1. **始终校验 srcStride × ndNum**：在 FixPipe 调用前使用 64 位整数计算乘积，确保不超 L1 容量
2. **srcStride 与 L0C 布局一致**：srcStride 应反映 L0C 中数据的实际存储 stride，而非随意赋值
3. **累积偏移跟踪**：在循环中多次调用 FixPipe 时，使用偏移跟踪器确保总偏移不超过 L1 限制
4. **tiling 参数校验**：在 tiling 模板中校验 srcStride × ndNum 不超过 L1 分配大小
5. **L0C 对齐要求**：srcStride 应满足 L0C 的对齐要求（通常 32 bytes），避免非对齐访问
6. **日志记录 srcStride**：在关键路径记录 srcStride 和 ndNum 的值，便于 L1 越界问题的快速定位

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| 扁平结构（常规变体） | stable_ez9999 历史种子（legacy: W5a） | `fp.srcStride = m * 100`（m=32, srcStride=3200）直接在 Process() 中设置 | EC2 |
| 深度嵌套（3 层函数传播） | stable_ez9999 历史种子（legacy: W5b） | `fp.srcStride = m * 100`（m=32, srcStride=3200）经 Process→RunPipeline→ExecuteStage(stageId==2) 传播 | EC3 |
| srcStride × ndNum 越界 | 变体（待生成） | srcStride 与 ndNum 乘积超出 L1 | EC2 |
| tiling 驱动的 srcStride | 变体（待生成） | tiling.srcStep 过大 | EC2 |
| 多次 FixPipe 累积偏移 | 变体（待生成） | 循环中 offset += srcStride 累积越界 | EC2 |

## 等价类划分

变异维度：FixPipe srcStride 相对于 L1 buffer 容量，以及参数传播深度

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | srcStride ≤ l1BufferSize 且 srcStride × ndNum ≤ l1BufferSize | 合法范围 | 读取范围在 L1 内，安全 |
| EC2 | srcStride 为编译期乘法常量过大值（扁平结构） | 如 m×100=3200 | 编译期可确定溢出，覆盖 seed-W5a |
| EC3 | srcStride 经多层函数传播后触发（深度嵌套） | 参数传播 ≥2 层 | 传播深度区分，覆盖 seed-W5b |

划分依据：FixPipe 对 L0C→L1 读取地址的范围检查。EC2/EC3 区分依据是参数传播深度（扁平 vs 深度嵌套）。

## 关联异常

- **fixpipe_gm_overflow**：共享 FixPipe 参数溢出模式，同属"FixPipe overflow"症状组（cross_references "症状相似"段：FixPipe overflow = {fixpipe_gm_overflow, fixpipe_cfg_illegal, fixpipe_l1_overflow, atomic_fixpipe_overflow}）。区分：fixpipe_gm_overflow 为 dstStride 驱动 GM 写溢出（错误码 141），fixpipe_l1_overflow 为 srcStride 驱动 L1 读越界。
- **fixpipe_cfg_illegal**：FixPipe 配置非法（ndNum/mSize/nSize 为 0），同属"FixPipe overflow"症状组（区分：fixpipe_cfg_illegal 为配置参数非法触发 fixp_err_illegal_cfg，fixpipe_l1_overflow 为 srcStride 驱动 L1 读越界）。
- **atomic_fixpipe_overflow**：FixPipe AtomicAdd + dstStride 组合导致 GM 溢出（错误码 141，与 fixpipe_gm_overflow 碰撞），同属"FixPipe overflow"症状组（区分：atomic_fixpipe_overflow 为 AtomicAdd 模式下 GM 写溢出，fixpipe_l1_overflow 为 srcStride 驱动 L1 读越界）。
- **tiling_overflow**：共享"扁平 vs 深度嵌套"变体模式和 L1 buffer 范围约束，tiling_overflow 为 Tiling 参数驱动 UB 溢出，fixpipe_l1_overflow 为 FixPipe srcStride 驱动 L1 溢出（语义相关，推断；tiling_overflow 不在 cross_references "症状相似" FixPipe overflow 组中）。
- **fixpipe_srcstride_overflow**：共享 FixPipe srcStride 参数族与 fixp_err_read_l1_ovflw 异常类型，fixpipe_srcstride_overflow 为深度链种子基于 fixpipe_gm_overflow 改造的读方向溢出。
- **共享参数**：（cross_references "参数共享"表收录）`srcStride → [fixpipe_l1_overflow, fixpipe_srcstride_overflow]`。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02）、变量名匹配（RULE-03）、存在性检查（RULE-04/05）
- **命中判定**：
  - 值域判断类规则——提取 `fp.srcStride = EXPR` 表达式，EXPR 含乘法且无 `min`/`ASSERT` 保护即标记风险；进一步计算 `srcStride × ndNum` 与 l1BufferSize 比对；跨行追溯 ≤3 层
  - 变量名匹配类规则——在源码定位 `offset += fp.srcStride` / `srcAddr += fp.srcStride` 等累积偏移变量，追溯循环结构；追溯受限时标记 `trace_limited=true`
  - 存在性检查类规则——全文模式搜索 FixPipe 调用前是否存在 `srcStride × ndNum <= l1Size` 断言；循环中累积偏移是否存在 `offset < L1_SIZE` 守卫（缺失即命中）
- **修复映射**：
  - fixpipe_l1_overflow-RULE-01（srcStride 乘法计算过大）→ 修复模式 1（srcStride 边界限制）
  - fixpipe_l1_overflow-RULE-02（srcStride × ndNum 越界）→ 修复模式 1 / 修复模式 4（L0C stride 对齐）
  - fixpipe_l1_overflow-RULE-03（累积偏移）→ 修复模式 3（累积偏移保护）
  - fixpipe_l1_overflow-RULE-04（L1 读取范围校验缺失）→ 修复模式 1
  - fixpipe_l1_overflow-RULE-05（累积偏移边界缺失）→ 修复模式 3
