# nd2nz_overflow — Nd2Nz 目标越界

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | nd2nz_overflow |
| 异常类型 | mte_write_overflow（Nd2Nz dstNzC0Stride 放大） |
| 错误码 | 48 |
| 类别 | memory_overflow |
| 源文件 | 来源: stable_ez9999 历史种子（legacy: W17） |
| 报错模式 | EZ9999_stderr |
| 内存区域 | L1（Nd2Nz 写入目标超出 A1 buffer 分配范围） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

当 Nd2Nz 数据搬运操作中 `dstNzC0Stride` 参数设置过大，导致写入目标地址超出 L1/A1 buffer 分配范围时，MTE 检测到写溢出并上报 nd2nz_overflow 异常（mte_write_overflow，错误码 48）。原 历史种子（legacy: W17） 使用 `dstNzC0Stride=256` 被 runtime 拦截；seed-W17s 使用 `dstNzC0Stride=65535` 穿透 runtime 检查，使 Nz 格式写入跨度 65535×sizeof(half)×C0 远超 A1 buffer 256×sizeof(half)=512B 分配，直接触发硬件异常。

- 违规模式（seed-W17s）：`ndA.dstNzC0Stride = 65535` — dstNzC0Stride=65535 使 Nz 格式写入跨度远超 A1 buffer 分配（512B），穿透 runtime 检查直接触发硬件异常（mte_write_overflow，错误码 48）
- 与原 历史种子（legacy: W17）（dstNzC0Stride=256 被 runtime 拦截）的区别：dstNzC0Stride=65535 穿透 runtime 检查直接触发硬件异常
- 安全模式：`ndA.dstNzC0Stride = ((128 + 15) / 16) * 16`（即 128，16 字节对齐后的合理值）
- 最小触发条件：Nd2NzParams.dstNzC0Stride 值使 Nz 格式写入总跨度超出 L1/buffer 分配范围

## 检测规则

### nd2nz_overflow-RULE-01：dstNzC0Stride 超大值检测

**【条件】**代码中出现 `ndParams.dstNzC0Stride = <大数值>`，dstNzC0Stride 的值使 `dstNzC0Stride × sizeof(element) × C0` 超出 L1 容量
**【API】**Nd2NzParams.dstNzC0Stride 赋值
**【约束】**`dstNzC0Stride × sizeof(element) × C0 <= L1_CAPACITY`
**【来源】**代码 diff 直接提取
**【匹配模式】** 值域判断

匹配示例（KB）：

- `ndParams.dstNzC0Stride = 2048`（超出 L1 容量）

匹配示例（detector 补充）：

```cpp
ndParams.dstNzC0Stride = 2048;
ndParams.dstNzC0Stride = 4096;
ndParams.dstNzC0Stride = LARGE_STRIDE;
```

### nd2nz_overflow-RULE-02：Nd2Nz 参数组合超限检测

**【条件】**`dstNzC0Stride × Nz × C0 × sizeof(element) > L1_CAPACITY`，多个 Nd2Nz 参数联合使写入总跨度超出 L1 容量
**【API】**Nd2NzParams 参数组合（dstNzC0Stride × nzShape × C0）
**【约束】**`dstNzC0Stride × Nz_count × C0 × sizeof(element) <= L1_CAPACITY`
**【来源】**API 文档约束
**【匹配模式】** 值域判断

匹配示例（detector 补充）：

```cpp
ndParams.dstNzC0Stride = 512;
ndParams.nzShape = {1, 256, 32, 16};
// 512 × 32 × 16 × 4 = 1MB > L1
```

### nd2nz_overflow-RULE-03：dstStride / dstNzNCStride 参数检测

**【条件】**`ndParams.dstStride` 或 `ndParams.dstNzNCStride` 赋值为过大值，或与 nzShape 组合后总写入超出 L1
**【API】**Nd2NzParams.dstStride / dstNzNCStride 赋值
**【约束】**`dstStride × Nz_count × sizeof(element) <= L1_CAPACITY` 且 `dstNzNCStride × N_count × sizeof(element) <= L1_CAPACITY`
**【来源】**LLM 推断
**【匹配模式】** 值域判断

匹配示例（detector 补充）：

```cpp
ndParams.dstNzNCStride = 2048 * 16;
```

### nd2nz_overflow-RULE-C0STRIDE：Nd2Nz dstNzC0Stride 超大值穿透

**【条件】**Nd2NzParams 的 dstNzC0Stride 字段为超大常量值（如 65535），且 buffer 分配大小远小于 dstNzC0Stride × sizeof(element) × C0，使 Nz 格式写入目标地址溢出 buffer 分配范围，穿透 runtime 检查直接触发硬件异常
**【API】**DataCopy / DataCopyPad（Nd2NzParams.dstNzC0Stride）
**【约束】**`dstNzC0Stride × sizeof(element) × C0 <= bufferAllocSize`
**【来源】**LLM 推断-已验证
**【匹配模式】** 值域判断

匹配示例（KB）：

- seed-W17s 中 `ndA.dstNzC0Stride = 65535` — A1 buffer 仅 256×sizeof(half)=512B
- 覆盖种子：stable_ez9999 历史种子（legacy: W17s）

## 修复方案

### 标准修复模式

1. **修复模式 1（dstNzC0Stride 值约束）**：根据 L1 容量和 Nz 参数反向计算最大允许 stride：`maxStride = L1_CAPACITY / (C0 × sizeof(element)); ndParams.dstNzC0Stride = std::min(desiredStride, maxStride)`
2. **修复模式 2（参数组合合法性校验）**：DataCopyPad 调用前验证 `dstNzC0Stride × Nz × C0 × sizeof(element) <= L1_CAPACITY`
3. **修复模式 3（tiling 驱动的 stride 安全计算）**：tiling 计算 stride 时引入 `maxAllowedStride = (L1_CAPACITY - SAFETY_MARGIN) / (C0 × sizeof(element))`
4. **修复模式 4（分块搬运替代单次大 stride）**：当单次搬运 stride 过大时，拆分为多次小 stride 搬运

### seed 具体修复

- seed-W17s：`ndA.dstNzC0Stride = 65535` → `ndA.dstNzC0Stride = ((128 + 15) / 16) * 16`（即 128）

### 代码模板

模板 1：安全的 Nd2Nz 参数计算函数（detector 补充）

```cpp
__aicore__ inline Nd2NzParams CalcSafeNd2NzParams(
    const LocalTensor<float>& dst,
    const GlobalTensor<float>& src,
    uint32_t nDim, uint32_t nzDim, uint32_t c0Dim) {
    constexpr uint64_t L1_CAPACITY = 1024 * 1024;
    Nd2NzParams params;
    params.nzShape = {1, nDim, nzDim, c0Dim};
    params.ndShape = {1, nDim, nzDim * c0Dim, c0Dim};
    uint64_t maxStride = L1_CAPACITY / (nzDim * c0Dim * sizeof(float));
    params.dstNzC0Stride = std::min(static_cast<uint64_t>(nzDim), maxStride);
    params.dstNzNCStride = params.dstNzC0Stride * c0Dim;
    params.srcNdStride = {nzDim * c0Dim, c0Dim, 1};
    return params;
}
```

### 防御性编程建议

1. **始终对 dstNzC0Stride 做上限约束**：根据 L1 容量和 Nz 参数反向计算最大允许 stride
2. **在 tiling 计算中加入 stride 约束**：tiling 算法应考虑 L1 容量限制，确保生成的 stride 参数合法
3. **添加运行时断言**：在 DataCopy/DataCopyPad 调用前添加 ASSERT 检查 stride 参数
4. **使用安全计算辅助函数**：封装 Nd2Nz 参数计算逻辑，集中处理边界情况
5. **考虑使用分块搬运**：当单次搬运 stride 过大时，拆分为多次小 stride 搬运
6. **保留安全裕量**：stride 计算时预留一定的安全裕量（如 4KB），避免边界情况

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| dstNzC0Stride 合理值（被拦截） | 原始 历史种子（legacy: W17）（TC-High） | `ndA.dstNzC0Stride = 256`，被 runtime 拦截 | EC2 |
| dstNzC0Stride 超大值穿透 | stable_ez9999 历史种子（legacy: W17s） | `ndA.dstNzC0Stride = 65535`，穿透 runtime（已验证） | EC3 |
| dstStride 过大 | 变体（待生成） | `ndParams.dstStride > L1_SIZE` | EC2 |
| 参数组合超 L1 | 变体（待生成） | `dstNzC0Stride × Nz × C0 × sizeof` 联合超限 | EC2 |
| tiling 驱动 stride 参数 | 变体（待生成） | tiling 计算未约束 stride 上限 | EC2 |

## 等价类划分

变异维度：Nd2NzParams.dstNzC0Stride × sizeof(element) × C0 相对于 buffer 分配大小

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | dstNzC0Stride × sizeof × C0 ≤ bufferAllocSize | 合理 stride 值 | 写入范围在 buffer 内，安全 |
| EC2 | dstNzC0Stride 为合理中等值（如 256） | 256 | 被 runtime 拦截，覆盖原始 历史种子（legacy: W17） |
| EC3 | dstNzC0Stride ≥ 65535 | ≥65535 | 超大值穿透 runtime，覆盖 seed-W17s（已验证） |

划分依据：MTE 对 Nd2Nz 写入目标地址的范围检查。EC2 为软件层拦截，EC3 为硬件层触发（已验证）。

## 关联异常

- **mte_write_ub_overflow/mte_read_ub_overflow**：共享 stride 参数超限穿透机制，mte_write_ub_overflow/mte_read_ub_overflow 为 DataCopy 的 dstStride/srcStride 穿透，nd2nz_overflow 为 Nd2Nz 的 dstNzC0Stride 穿透。
- **ub_capacity_overflow**：共享 buffer 容量约束，ub_capacity_overflow 为 UB 静态声明超容，nd2nz_overflow 为 Nd2Nz 动态写入超容。
- **共享参数**：（推断，非 cross_references "参数共享"表收录）与 mte_write_ub_overflow/mte_read_ub_overflow 共享 stride 参数超限穿透模式；与 ub_capacity_overflow 共享 buffer 容量约束。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02/03/C0STRIDE）
- **命中判定**：
  - 值域判断类规则——提取 `ndParams.dstNzC0Stride = VALUE` 表达式，VALUE 为编译期常量且使 `VALUE × sizeof(element) × C0 > L1_CAPACITY` 即命中；dstNzC0Stride ≥ 65535 直接判定穿透；参数组合场景计算 `dstNzC0Stride × Nz × C0 × sizeof(element)` 与 L1_CAPACITY 比对；跨行追溯 ≤3 层
- **修复映射**：
  - nd2nz_overflow-RULE-01（dstNzC0Stride 超大值）→ 修复模式 1（值约束）
  - nd2nz_overflow-RULE-02（参数组合超限）→ 修复模式 2（参数组合校验）
  - nd2nz_overflow-RULE-03（dstStride / dstNzNCStride）→ 修复模式 1 / 修复模式 2
  - nd2nz_overflow-RULE-C0STRIDE（超大值穿透）→ 修复模式 1 / 修复模式 4（分块搬运）
