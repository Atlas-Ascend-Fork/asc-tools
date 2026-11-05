# fixpipe_gm_overflow — FixPipe 写入 GM 溢出

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | fixpipe_gm_overflow |
| 异常类型 | fixp_err_out_write_overflow |
| 错误码 | 141（与 atomic_fixpipe_overflow 共享，碰撞） |
| 类别 | fixpipe |
| 源文件 | cann-aicore/10_fixpipe/fixpipe_write_oob_verified/error/main.asc |
| 报错模式 | EZ9999_stderr |
| 内存区域 | GM（FixPipe 输出写入 GM 侧超出 buffer 边界） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

在 FixPipe 操作中，`dstStride` 被设置为过大的值，或 `ndNum` 过多，导致写入地址计算结果超出 GM 中分配的 buffer 范围。硬件检测到写入溢出，触发 fixp_err_out_write_overflow 异常（错误码 141，与 atomic_fixpipe_overflow 碰撞），可能导致 GM 中其他数据被破坏。同属 cross_references "症状相似"段：FixPipe overflow = {fixpipe_gm_overflow, fixpipe_cfg_illegal, fixpipe_l1_overflow, atomic_fixpipe_overflow}（区分要点：fixpipe_gm_overflow = dstStride 驱动 GM 写溢出）。

触发条件（detector 原文，逐字保留）：

- C1：`dstStride × ndNum > gmBufferSize`（累积写入超出 GM）
- C2：`dstStride` 过大（stride 本身超过 buffer）
- C3：`ndNum` 过大（循环次数过多）
- C4：tiling 驱动的 stride 参数（tiling 计算 stride 错误）

形式化规则前提：`ASSERT_fixpipe_gm_overflow_1: static_cast<uint64_t>(dstStride) * ndNum <= gmBufferSize`。

## 检测规则

### fixpipe_gm_overflow-RULE-01：dstStride 乘积计算溢出

**【条件】**代码中出现 `dstStride = EXPR * CONSTANT`（如 `fp.dstStride = n * 100`、`fp.dstStride = cols * sizeof(float) * blockDim.x`）且无 `min` 或 `ASSERT` 保护
**【API】**FixPipeParams.dstStride 赋值（乘法表达式）
**【约束】**`dstStride × ndNum <= gmBufferSize`，且赋值表达式存在 `min`/`ASSERT` 保护
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
fp.dstStride = n * 100;
fp.dstStride = cols * sizeof(float) * blockDim.x;
```

### fixpipe_gm_overflow-RULE-02：ndNum 与 buffer 容量不匹配

**【条件】**`ndNum = LARGE_VALUE`（如 128、10000）且 `dstStride × ndNum` 可能超 GM buffer
**【API】**FixPipeParams.ndNum 赋值
**【约束】**`ndNum <= gmBufferSize / max(1, dstStride)`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 值域判断

匹配示例（detector 原文）：

```cpp
fp.ndNum = 128;
fp.ndNum = totalRows;
```

### fixpipe_gm_overflow-RULE-03：tiling 驱动的 stride 计算

**【条件】**`dstStride = tiling.FIELD` 或 `dstStride = blockIdx.x * tileLen`，dstStride 来自 tiling 参数且缺少校验
**【API】**FixPipeParams.dstStride 赋值（tiling/blockIdx 驱动）
**【约束】**`tiling.outStride × tiling.ndNum <= gmAllocSize`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 变量名匹配

匹配示例（detector 原文）：

```cpp
fp.dstStride = tiling.outStride;
fp.dstStride = blockIdx.x * tileLen;
```

### fixpipe_gm_overflow-RULE-04：写入范围校验缺失（运行时检测）

**【条件】**FixPipe 调用前缺少 `dstStride × ndNum <= gmSize` 断言
**【API】**FixPipe（调用前范围校验）
**【约束】**FixPipe 调用前存在 `static_cast<uint64_t>(dstStride) * ndNum <= gmBufferSize` 断言
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

### fixpipe_gm_overflow-RULE-05：uint32 溢出检测（运行时检测）

**【条件】**`dstStride × ndNum` 计算未使用 64 位整数防止溢出
**【API】**FixPipeParams.dstStride × FixPipeParams.ndNum（乘法类型）
**【约束】**乘法使用 `uint64_t` 计算避免 32 位溢出
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

## 修复方案

### 标准修复模式

1. **修复模式 1（dstStride × ndNum 边界限制）**：`uint64_t totalWrite = static_cast<uint64_t>(n) * 100 * 128; ASSERT(totalWrite <= gmBufferSize);`
2. **修复模式 2（ndNum 自适应限制）**：`uint32_t maxNd = stride > 0 ? gmBufferSize / stride : 0; fp.ndNum = std::min(totalRows, maxNd);`
3. **修复模式 3（tiling 驱动的 stride 安全计算）**：`uint64_t maxStride = gmBufferSize / std::max(fp.ndNum, 1u); fp.dstStride = std::min(tiling.outStride, maxStride);`
4. **修复模式 4（输出 tensor stride 对齐）**：`uint32_t alignedStride = ((n * sizeof(float) + 31) / 32) * 32; fp.dstStride = std::min(alignedStride, maxStride);`

### 代码模板

模板 1：安全 FixPipe 参数计算（detector 原文）

```cpp
template<typename T>
inline FixPipeParams<T> SafeFixPipeParams(uint32_t stride, uint32_t ndNum, uint64_t gmSize) {
    uint32_t maxNd = stride > 0 ? static_cast<uint32_t>(gmSize / stride) : 0;
    FixPipeParams<T> fp;
    fp.dstStride = stride;
    fp.ndNum = std::min(ndNum, maxNd);
    ASSERT(static_cast<uint64_t>(fp.dstStride) * fp.ndNum <= gmSize);
    return fp;
}
```

模板 2：GM 写入范围校验器（detector 原文）

```cpp
inline void ValidateGmWriteRange(uint32_t stride, uint32_t ndNum, uint64_t gmSize) {
    uint64_t total = static_cast<uint64_t>(stride) * ndNum;
    ASSERT(total <= gmSize);
}
```

### 防御性编程建议

1. **始终使用 64 位乘法校验**：dstStride × ndNum 使用 `uint64_t` 计算，避免 32 位溢出导致误判。
2. **限制 ndNum 上限**：在设置 ndNum 前计算 `gmSize / dstStride` 获取最大允许值。
3. **stride 与 tensor 实际布局一致**：确保 dstStride 与输出 tensor 的 stride 匹配，而非随意赋值。
4. **tiling 参数校验**：在 tiling 模板中校验 outStride × ndNum 不超过 GM 分配大小。
5. **输出 buffer 预留余量**：GM buffer 分配时预留额外空间，避免边界写入溢出。
6. **日志记录关键参数**：在 FixPipe 调用前记录 dstStride、ndNum 和 gmSize，便于问题定位。

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| dstStride 过大 | cann-aicore/10_fixpipe/fixpipe_write_oob_verified | `fp.dstStride = n * 100`，stride 本身超过 buffer | EC2 |
| ndNum 过大 | 变体（待生成） | `fp.ndNum = 10000`，循环次数过多 | EC2 |
| dstStride × ndNum 超出 GM | 变体（待生成） | stride=25600, ndNum=100，乘积溢出 | EC2 |
| tiling 驱动的 stride 参数 | 变体（待生成） | tiling.outStride 过大 | EC2 |

## 等价类划分

变异维度：FixPipeParams.dstStride × ndNum 相对于 GM buffer 容量

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | dstStride × ndNum ≤ gmBufferSize | 合法范围 | 写入范围在 GM 内，安全 |
| EC2 | dstStride × ndNum > gmBufferSize | stride 或 ndNum 任意一项使乘积溢出 | 编译期常量可确定溢出，覆盖原始种子 |

划分依据：FixPipe 对 GM 写入地址的范围检查。原始种子为编译期可确定的乘积溢出（dstStride = n * 100），未观察到深度嵌套/tiling 驱动等区分性变体，故统一归为 EC2。

## 关联异常

- **fixpipe_cfg_illegal / fixpipe_l1_overflow / atomic_fixpipe_overflow**：同属 cross_references "症状相似"段：FixPipe overflow = {fixpipe_gm_overflow, fixpipe_cfg_illegal, fixpipe_l1_overflow, atomic_fixpipe_overflow}。区分要点（cross_references 原文）：fixpipe_gm_overflow = FixPipe 写 GM 溢出（错误码 141）；fixpipe_cfg_illegal = FixPipe 配置非法；fixpipe_l1_overflow = FixPipe 读 L1 越界；atomic_fixpipe_overflow = FixPipe AtomicAdd GM 溢出（错误码 141，与 fixpipe_gm_overflow 碰撞）。
- **atomic_fixpipe_overflow**：错误码 141 双重关联——同症状组 + 同错误码碰撞（error_code_index.md §碰撞标注：141 → [fixpipe_gm_overflow, atomic_fixpipe_overflow]）。诊断时需先检查是否存在 SetAtomicAdd 调用以区分 atomic_fixpipe_overflow 与 fixpipe_gm_overflow。
- **FixPipe**：共享 API（cross_references 反查 api_index：FixPipe → [fixpipe_gm_overflow, fixpipe_cfg_illegal, fixpipe_l1_overflow, atomic_fixpipe_overflow, fixpipe_srcstride_overflow]）。
- **（推断，非 cross_references 收录）**与 fixpipe_l1_overflow 共享 FixPipe 参数溢出模式，fixpipe_gm_overflow 为 dstStride 驱动 GM 写溢出，fixpipe_l1_overflow 为 srcStride 驱动 L1 读越界；cross_references "参数共享"表 `srcStride → [fixpipe_l1_overflow, fixpipe_srcstride_overflow]`，fixpipe_gm_overflow 不在该条目中。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02）、变量名匹配（RULE-03）、存在性检查（RULE-04/05）
- **命中判定**：
  - 值域判断类规则——提取 `fp.dstStride = EXPR` / `fp.ndNum = VALUE` 表达式，EXPR 含乘法或 VALUE 为编译期常量；进一步计算 `static_cast<uint64_t>(dstStride) × ndNum` 与 gmBufferSize 比对；超限即命中；跨行追溯 ≤3 层
  - 变量名匹配类规则——在源码定位 `fp.dstStride = tiling.FIELD` / `blockIdx.x * tileLen` 表达式，追溯 tiling 字段或 blockIdx 计算；追溯受限时标记 `trace_limited=true`
  - 存在性检查类规则——全文模式搜索 FixPipe 调用前是否存在 `dstStride × ndNum <= gmSize` 断言；乘法表达式是否使用 uint64_t 强转（缺失即命中）
- **修复映射**：
  - fixpipe_gm_overflow-RULE-01（dstStride 乘积溢出）→ 修复模式 1（边界限制）/ 修复模式 4（stride 对齐）
  - fixpipe_gm_overflow-RULE-02（ndNum 与容量不匹配）→ 修复模式 2（ndNum 自适应限制）
  - fixpipe_gm_overflow-RULE-03（tiling 驱动 stride）→ 修复模式 3（tiling 安全计算）
  - fixpipe_gm_overflow-RULE-04（写入范围校验缺失）→ 修复模式 1 / 修复模式 2（添加 ASSERT）
  - fixpipe_gm_overflow-RULE-05（uint32 溢出）→ 修复模式 1（使用 uint64_t）
