# atomic_fixpipe_overflow — AtomicAdd FixPipe GM 溢出

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | atomic_fixpipe_overflow |
| 异常类型 | fixp_err_out_write_overflow |
| 错误码 | 141（与 fixpipe_gm_overflow 共享，碰撞） |
| 类别 | fixpipe |
| 源文件 | cann-aicore/10_fixpipe/fixpipe_atomic_oob_verified/error/main.asc |
| 报错模式 | EZ9999_stderr |
| 内存区域 | GM（FixPipe + AtomicAdd 组合写入 GM 侧溢出） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

在 FixPipe 与 AtomicAdd 组合使用场景中，先调用 `SetAtomicAdd` 设置原子加操作的目标地址和参数，随后执行 `FixPipe` 时 `dstStride` 过大。AtomicAdd 的目标地址计算与 FixPipe 的输出地址叠加，导致写入范围超出 GM buffer，触发 fixp_err_out_write_overflow 异常（错误码 141，与 fixpipe_gm_overflow 碰撞）。同属 cross_references "症状相似"段：FixPipe overflow = {fixpipe_gm_overflow, fixpipe_cfg_illegal, fixpipe_l1_overflow, atomic_fixpipe_overflow}（区分要点：atomic_fixpipe_overflow = AtomicAdd 模式下 GM 写溢出，错误码 141 与 fixpipe_gm_overflow 碰撞）。

触发条件（detector 原文，逐字保留）：

- C1：`atomicOffset + dstStride × ndNum > gmSize`（AtomicAdd 偏移加 FixPipe 写入超限）
- C2：`dstStride` 在 AtomicAdd 上下文中过大（stride 未考虑 AtomicAdd 额外偏移）
- C3：AtomicAdd 地址计算越界（AtomicAdd 自身地址参数过大）
- C4：与 fixpipe_gm_overflow 区分：本异常有 AtomicAdd 上下文（fixpipe_gm_overflow 无 AtomicAdd）

形式化规则前提：`ASSERT_atomic_fixpipe_overflow_1: atomicOffset + dstStride × ndNum <= gmBufferSize`；`ASSERT_atomic_fixpipe_overflow_4: has_SetAtomicAdd_call == true`（区分 fixpipe_gm_overflow）。

## 检测规则

### atomic_fixpipe_overflow-RULE-01：SetAtomicAdd + FixPipe 组合缺校验

**【条件】**代码中出现 `SetAtomicAdd ... FixPipe` 序列且无 `ASSERT.*gmSize` 或 `ASSERT.*buffer` 校验
**【API】**SetAtomicAdd / FixPipe（组合调用）
**【约束】**AtomicAdd 与 FixPipe 组合使用时存在 `atomicOffset + dstStride × ndNum <= gmBufferSize` 校验
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

匹配示例（detector 原文）：

```cpp
SetAtomicAdd<float>(cGM, 1.0f, atParams);
fp.dstStride = n * 100;
FixPipe(fp);
```

### atomic_fixpipe_overflow-RULE-02：AtomicAdd 偏移与 dstStride 叠加越界

**【条件】**`SetAtomicAdd ... offset ... dstStride = EXPR × CONST`，AtomicAdd 偏移与 dstStride 乘积可能超过 GM
**【API】**SetAtomicAdd.offset × FixPipeParams.dstStride × ndNum
**【约束】**`atomicOffset + dstStride × ndNum <= gmBufferSize`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
SetAtomicAdd<float>(cGM, 1.0f, atParams);
fp.dstStride = n * 100;
fp.ndNum = 200;
```

### atomic_fixpipe_overflow-RULE-03：多次 AtomicAdd + FixPipe 累积

**【条件】**`for { SetAtomicAdd ... FixPipe }` 循环内组合调用，循环中多次 AtomicAdd + FixPipe 累积偏移越界
**【API】**SetAtomicAdd / FixPipe（循环内组合）
**【约束】**`Σ(atomicOffset_i + dstStride_i × ndNum_i) <= gmBufferSize`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

匹配示例（detector 原文）：

```cpp
for (int i = 0; i < iters; i++) {
    SetAtomicAdd<float>(cGM, 1.0f, atParams);
    fp.dstStride = stride;
    FixPipe(fp);
}
```

### atomic_fixpipe_overflow-RULE-04：AtomicAdd 偏移 + 写入范围校验缺失（运行时检测）

**【条件】**SetAtomicAdd + FixPipe 组合中缺少 `atomicOffset + dstStride × ndNum <= gmSize` 断言
**【API】**SetAtomicAdd / FixPipe（组合范围校验）
**【约束】**组合中存在 `ASSERT(atomicOffset + dstStride × ndNum <= gmSize)`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

### atomic_fixpipe_overflow-RULE-05：与 fixpipe_gm_overflow 异常区分（运行时检测）

**【条件】**fixp_err_out_write_overflow（错误码 141）上下文中无 SetAtomicAdd 调用（应路由到 fixpipe_gm_overflow）
**【API】**SetAtomicAdd（上下文存在性）
**【约束】**atomic_fixpipe_overflow 诊断需 `has_SetAtomicAdd_call == true`，否则路由到 fixpipe_gm_overflow
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

## 修复方案

### 标准修复模式

1. **修复模式 1（AtomicAdd 偏移 + dstStride 联合校验）**：`uint32_t availableSpace = GM_SIZE - atParams.offset; uint32_t maxStride = fp.ndNum > 0 ? availableSpace / fp.ndNum : 0; fp.dstStride = std::min(n * 100, maxStride); ASSERT(...);`
2. **修复模式 2（AtomicAdd 偏移安全计算）**：`uint64_t rawOffset = static_cast<uint64_t>(blockIdx.x) * largeStride; atParams.offset = static_cast<uint32_t>(std::min(rawOffset, static_cast<uint64_t>(GM_SIZE) - minFixPipeSize));`
3. **修复模式 3（多次调用累积偏移保护）**：循环中添加 `if (offset + static_cast<uint64_t>(fp.dstStride) * fp.ndNum >= GM_SIZE) break;` 守卫
4. **修复模式 4（AtomicAdd + FixPipe 安全封装）**：封装 `SafeAtomicFixPipe` 工具函数统一校验

### 代码模板

模板 1：AtomicAdd + FixPipe 参数校验器（detector 原文）

```cpp
struct AtomicFixPipeValidator {
    static void Validate(const AtomicParams& at, const FixPipeParams<float>& fp, uint64_t gmSize) {
        ASSERT(at.offset < gmSize);
        ASSERT(static_cast<uint64_t>(at.offset) + static_cast<uint64_t>(fp.dstStride) * fp.ndNum <= gmSize);
    }
};
```

模板 2：atomic_fixpipe_overflow vs fixpipe_gm_overflow 异常区分辅助（detector 原文）

```cpp
inline bool IsAtomicFixPipeOverflow(const KernelContext& ctx) {
    return ctx.HasSetAtomicAdd() && ctx.GetErrorCode() == 141;
}
```

### 防御性编程建议

1. **AtomicAdd 与 FixPipe 联动校验**：在 SetAtomicAdd + FixPipe 组合中，将 atomicOffset 纳入总写入范围的计算。
2. **使用 64 位整数计算**：atomicOffset + dstStride × ndNum 使用 `uint64_t` 计算，避免 32 位溢出。
3. **区分 atomic_fixpipe_overflow 与 fixpipe_gm_overflow**：相同错误码 141，但 atomic_fixpipe_overflow 存在 AtomicAdd 上下文。诊断时应先检查是否有 SetAtomicAdd 调用。
4. **循环累积偏移保护**：在多次 AtomicAdd + FixPipe 循环中跟踪累积偏移，设置安全退出条件。
5. **AtomicAdd 偏移前置校验**：在 SetAtomicAdd 前校验 offset 本身不超过 GM 大小。
6. **单元测试覆盖组合场景**：单独测试 AtomicAdd 和 FixPipe 各自合法但组合溢出的边界场景。

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| AtomicAdd + dstStride 溢出 | cann-aicore/10_fixpipe/fixpipe_atomic_oob_verified | SetAtomicAdd + dstStride = n*100，组合溢出 | EC2 |
| AtomicAdd 地址计算越界 | 变体（待生成） | atParams.offset 过大 | EC2 |
| 与 fixpipe_gm_overflow 区分（fixpipe_gm_overflow 无 AtomicAdd） | 变体（待生成） | 本变体有 SetAtomicAdd 调用 | - |
| AtomicAdd 多次调用累积 | 变体（待生成） | 多次 SetAtomicAdd + FixPipe，累积偏移超限 | EC3 |

## 等价类划分

变异维度：SetAtomicAdd + FixPipe 组合的 atomicOffset + dstStride × ndNum 相对于 GM 容量

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | atomicOffset + dstStride × ndNum ≤ gmBufferSize | 合法范围 | 写入范围在 GM 内，安全 |
| EC2 | 单次组合使 atomicOffset + dstStride × ndNum > gmBufferSize | 编译期可确定 | 覆盖原始种子 |
| EC3 | 多次循环累积偏移越界 | Σ(...) > gmBufferSize | 需循环迭代分析 |

划分依据：FixPipe + AtomicAdd 组合对 GM 写入地址的范围检查。EC2 为编译期可确定的单次溢出，EC3 需循环迭代累积分析。

## 关联异常

- **fixpipe_gm_overflow / fixpipe_cfg_illegal / fixpipe_l1_overflow**：同属 cross_references "症状相似"段：FixPipe overflow = {fixpipe_gm_overflow, fixpipe_cfg_illegal, fixpipe_l1_overflow, atomic_fixpipe_overflow}。区分要点（cross_references 原文）：fixpipe_gm_overflow = FixPipe 写 GM 溢出（错误码 141）；fixpipe_cfg_illegal = FixPipe 配置非法（错误码 101）；fixpipe_l1_overflow = FixPipe 读 L1 越界（错误码 103）；atomic_fixpipe_overflow = FixPipe AtomicAdd GM 溢出（错误码 141，与 fixpipe_gm_overflow 碰撞）。
- **fixpipe_gm_overflow**：错误码 141 双重关联——同症状组 + 同错误码碰撞（error_code_index.md §碰撞标注：141 → [fixpipe_gm_overflow, atomic_fixpipe_overflow]，均为 fixp_err_out_write_overflow）。诊断区分：atomic_fixpipe_overflow 存在 SetAtomicAdd 调用，fixpipe_gm_overflow 无。
- **FixPipe**：共享 API（cross_references 反查 api_index：FixPipe → [fixpipe_gm_overflow, fixpipe_cfg_illegal, fixpipe_l1_overflow, atomic_fixpipe_overflow, fixpipe_srcstride_overflow]）；**AtomicAdd**：共享关键词（cross_references 反查 keyword_index：AtomicAdd → [atomic_fixpipe_overflow]）。
- **（推断，非 cross_references 收录）**与 fixpipe_gm_overflow 共享 dstStride × ndNum 溢出机制，但 atomic_fixpipe_overflow 多一层 AtomicAdd 偏移叠加；修复时需将 atomicOffset 纳入总写入范围。

## 检测执行指引

- **适用匹配模式**：API 调用模式（RULE-01/03）、值域判断（RULE-02）、存在性检查（RULE-04/05）
- **命中判定**：
  - API 调用模式类规则——构建 SetAtomicAdd → FixPipe 调用序列，循环内统计累积偏移；序列中存在 SetAtomicAdd 紧接 FixPipe 且无范围校验即命中 RULE-01；循环内累积偏移超 GM 即命中 RULE-03；CFG 跨函数追溯受限时标记 `trace_limited=true`
  - 值域判断类规则——提取 `atParams.offset` 与 `fp.dstStride × fp.ndNum`，计算 `offset + dstStride × ndNum` 与 gmBufferSize 比对；超限即命中 RULE-02；跨行追溯 ≤3 层
  - 存在性检查类规则——全文模式搜索 SetAtomicAdd + FixPipe 组合中是否存在 `atomicOffset + dstStride × ndNum <= gmSize` 断言（缺失即命中 RULE-04）；错误码 141 上下文中是否存在 SetAtomicAdd 调用（缺失则路由到 fixpipe_gm_overflow，命中 RULE-05）
- **修复映射**：
  - atomic_fixpipe_overflow-RULE-01（组合缺校验）→ 修复模式 1（联合校验）/ 修复模式 4（安全封装）
  - atomic_fixpipe_overflow-RULE-02（偏移叠加越界）→ 修复模式 1 / 修复模式 2（偏移安全计算）
  - atomic_fixpipe_overflow-RULE-03（多次累积）→ 修复模式 3（累积偏移保护）
  - atomic_fixpipe_overflow-RULE-04（范围校验缺失）→ 修复模式 1（添加 ASSERT）
  - atomic_fixpipe_overflow-RULE-05（与 fixpipe_gm_overflow 区分）→ 用于诊断路由，结合 SetAtomicAdd 存在性判定
