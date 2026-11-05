# tiling_overflow — Tiling 搬运溢出

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | tiling_overflow |
| 异常类型 | Tiling搬运溢出 |
| 错误码 | 无（EZ9999 报错无数值型错误码） |
| 类别 | alignment_config |
| 源文件 | 来源: stable_ez9999 历史种子（legacy: F1） |
| 报错模式 | EZ9999_stderr |
| 内存区域 | UB（通过 GM→UB 搬运溢出） |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

从 Host 传递到 Kernel 的 Tiling 参数中，buffer size 相关字段（如 `copyBytes`）被设置为超大值，导致 MTE（Memory Transfer Engine）在执行 GM→UB 搬运时访问的地址超出 UB buffer 分配范围。历史种子（legacy: F1） 中 `tiling.copyBytes = 999999` 直接传入 `asc_copy_gm2ub_sync`，seed-F1d 中同值经 3 层函数调用链（`Process → ProcessTiles → ProcessOneTile`）传播后在 `DataCopy` 中触发。两者根因相同（Tiling 参数超大），但代码结构差异显著（扁平 vs 深度嵌套）。

- 违规模式（历史种子（legacy: F1） 扁平）：`tiling.copyBytes = 999999`，直接传入 `asc_copy_gm2ub_sync`，搬运 999999 字节 >> UB_BUF_SIZE（64 float = 256 字节）
- 违规模式（seed-F1d 深度嵌套）：`TilingData.copyBytes = 999999`，经 `Process → ProcessTiles → ProcessOneTile` 3 层传播，最终在 `DataCopy(xLocal, xGlobal, copyLen)` 中触发（copyLen = 999999/4 = 249999 个 float >> UB_BUF_SIZE=64）
- 安全模式：`tiling.copyBytes = UB_BUF_SIZE * sizeof(float)`（即 256）
- 最小触发条件：Tiling 参数中搬运大小字段超出 UB buffer 分配容量，MTE 执行 GM→UB 搬运时地址越界触发 `mte access out of range`

## 检测规则

### tiling_overflow-RULE-01：TilingStruct buffer size 字段超大常量

**【条件】**代码中出现 `tiling.FIELD = LARGE_CONSTANT`（如 `tiling.copyBytes = 999999`、`tiling.srcBufSize = 0xFFFFFFFF`），FIELD 表示 buffer size 或搬运字节数，常量值超过合理上限（如 > UB_SIZE 或 > 实际分配大小）
**【API】**Tiling 参数赋值（`tiling.copyBytes` / `tiling.srcBufSize` / `tiling.dstBufSize`）
**【约束】**`FIELD <= bufferAllocSize`
**【来源】**代码 diff 直接提取
**【匹配模式】** 值域判断

匹配示例（KB）：

- 历史种子（legacy: F1） 中 `tiling.copyBytes = 999999` >> UB_BUF_SIZE * sizeof(float) = 256

匹配示例（detector 补充）：

```cpp
tiling.srcBufSize = 0xFFFFFFFF;
tiling.dstBufSize = 0xFFFFFFFF;
```

### tiling_overflow-RULE-02：乘法运算潜在溢出

**【条件】**代码中出现 `tiling.totalSize = paramA * paramB` 或 `tiling.bufSize = elements * sizeof(T)`，乘法运算可能溢出 uint32 或超出 buffer 容量，且缺少溢出检查
**【API】**Tiling 参数计算（乘法表达式）
**【约束】**`A <= UINT32_MAX / B` 且 `A × B <= bufferCapacity`
**【来源】**API 文档约束
**【匹配模式】** 值域判断

匹配示例（detector 补充）：

```cpp
tiling.srcBufSize = batchSize * channelSize * height * width;
```

### tiling_overflow-RULE-03：Tiling 参数未初始化

**【条件】**TilingStruct 声明后未在首次使用前完成所有字段赋值，或存在可能跳过初始化的执行路径
**【API】**Tiling 参数使用（`DataCopy` / `asc_copy_gm2ub_sync` 前引用 tiling 字段）
**【约束】**所有 TilingStruct 字段在首次使用前已被赋值且赋值来源经过范围校验
**【来源】**LLM 推断
**【匹配模式】** 存在性检查

匹配示例（detector 补充）：

```cpp
struct TilingData tiling;
// 未初始化即使用 tiling.srcBufSize
DataCopy(dstLocal, srcGlobal, tiling.srcBufSize);
```

### tiling_overflow-RULE-04：Tiling 参数范围校验缺失

**【条件】**Host 侧计算 Tiling 时缺少 `tiling.srcBufSize <= UB_CAPACITY` 或 `tiling.copyBytes <= UB_BUF_SIZE * sizeof(T)` 的范围校验
**【API】**Host 侧 Tiling 计算函数
**【约束】**`tiling.FIELD <= corresponding_buffer_capacity`
**【来源】**LLM 推断
**【匹配模式】** 存在性检查

### tiling_overflow-RULE-DEPTH：多层函数调用链中的 DataCopy 使用 tiling 参数（深度嵌套）

**【条件】**多层函数调用链（≥2 层）中的 DataCopy 使用来自 tiling 参数的计算值，参数传播跨越 ≥2 层函数调用（如 `Process → ProcessTiles → ProcessOneTile`），且最终 copyLen 超出 buffer 大小
**【API】**DataCopy（经多层函数调用传播的 tiling 参数）
**【约束】**`copyLen <= bufferAllocSize / sizeof(T)`
**【来源】**运行验证
**【匹配模式】** API 调用模式

匹配示例（KB）：

- seed-F1d 中 `tiling.copyBytes = 999999` 经 3 层传播后在 `ProcessOneTile` 的 `DataCopy(xLocal, xGlobal, copyLen)` 中触发，copyLen = 999999/4 = 249999 >> UB_BUF_SIZE=64
- 覆盖种子：stable_ez9999 历史种子（legacy: F1d）

## 修复方案

### 标准修复模式

1. **修复模式 1（限制 tiling buffer size 上限）**：`tiling.copyBytes = std::min(totalSize, MAX_BUFFER_SIZE)`，MAX_BUFFER_SIZE 为 UB buffer 实际分配大小
2. **修复模式 2（乘法溢出保护）**：涉及乘法运算时先提升到 uint64，运算后截断并校验 `result <= bufferCapacity`
3. **修复模式 3（Tiling 参数初始化与校验）**：声明 TilingStruct 时零初始化，计算后添加范围断言
4. **修复模式 4（Host 侧参数校验）**：在 tiling 计算完成后执行 `ASSERT(tiling.copyBytes > 0 && tiling.copyBytes <= MAX_SIZE)`

### seed 具体修复

- 历史种子（legacy: F1）：`tiling.copyBytes = 999999` → `tiling.copyBytes = UB_BUF_SIZE * sizeof(float)`（即 256）
- seed-F1d：`TilingData tiling = {256, 999999, 1}` → `TilingData tiling = {256, UB_BUF_SIZE * sizeof(float), 1}`（即 copyBytes = 256）

### 代码模板

模板 1：安全的 Tiling 参数计算（detector 补充）

```cpp
struct SafeTilingData {
    uint32_t srcBufSize;
    uint32_t dstBufSize;
    uint32_t blockCount;
    uint32_t blockSize;
};

void ComputeSafeTiling(SafeTilingData& tiling, uint32_t totalElements, uint32_t elementSize) {
    const uint32_t MAX_UB_SIZE = 1024 * 1024;
    const uint32_t MAX_SINGLE_BUF = MAX_UB_SIZE / 2;

    uint64_t totalBytes = (uint64_t)totalElements * elementSize;
    tiling.srcBufSize = static_cast<uint32_t>(std::min(totalBytes, (uint64_t)MAX_SINGLE_BUF));
    tiling.dstBufSize = tiling.srcBufSize;
    tiling.blockSize = 256;
    tiling.blockCount = (tiling.srcBufSize + tiling.blockSize - 1) / tiling.blockSize;
}
```

模板 2：Host 侧参数校验框架（detector 补充）

```cpp
bool ValidateTilingParams(const SafeTilingData& tiling) {
    const uint32_t MAX_UB_SIZE = 1024 * 1024;
    if (tiling.srcBufSize == 0 || tiling.srcBufSize > MAX_UB_SIZE) return false;
    if (tiling.dstBufSize == 0 || tiling.dstBufSize > MAX_UB_SIZE) return false;
    if (tiling.blockCount == 0) return false;
    if (tiling.blockSize == 0) return false;
    return true;
}
```

### 防御性编程建议

1. **统一上限常量定义**：定义 `MAX_UB_SIZE` 常量，所有 tiling buffer size 计算引用此上限
2. **使用 64 位中间变量**：涉及乘法运算时先提升到 uint64，运算后再截断并校验
3. **零初始化 TilingStruct**：声明时使用 `= {}` 确保所有字段初始化为 0
4. **Host 侧参数校验**：在 tiling 计算完成后、写入 device 前执行完整性检查
5. **Kernel 入口运行时断言**：kernel 入口处对关键 tiling 参数增加运行时断言
6. **乘法前检查溢出**：执行 `a * b` 前判断 `a <= UINT32_MAX / b`

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| 扁平结构（直接传入搬运 API） | stable_ez9999 历史种子（legacy: F1） | `tiling.copyBytes = 999999` 直接传入 `asc_copy_gm2ub_sync` | EC2 |
| 深度嵌套（3 层函数传播） | stable_ez9999 历史种子（legacy: F1d） | `TilingData.copyBytes = 999999` 经 Process→ProcessTiles→ProcessOneTile 3 层传播 | EC3 |
| srcBufSize 过大 | 变体（待生成） | `tiling.srcBufSize = 0xFFFFFFFF` | EC2 |
| dstBufSize 过大 | 变体（待生成） | `tiling.dstBufSize = 0xFFFFFFFF` | EC2 |
| 多参数组合超大 | 变体（待生成） | `tiling.blockCount × tiling.blockSize` 结果溢出 uint32 | EC2 |
| tiling 计算溢出 | 变体（待生成） | host 侧乘法 `a * b` 在 a、b 较大时超过 UINT32_MAX | EC2 |

## 等价类划分

变异维度：Tiling 搬运参数 copyBytes 相对于 buffer 容量 BUF_SIZE

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | copyBytes ≤ BUF_SIZE × sizeof(T) | 0 ~ BUF_SIZE×sizeof(T) | 搬运范围在 buffer 内，安全 |
| EC2 | copyBytes 为编译期常量大值 | 如 999999 | 编译期可确定溢出，覆盖 历史种子（legacy: F1） 扁平结构 |
| EC3 | copyBytes 经多层函数传播 | tiling 驱动的运行时值 | 参数传播 ≥2 层函数调用，覆盖 seed-F1d 深度嵌套 |

划分依据：MTE 对 UB buffer 访问地址的范围检查。EC2/EC3 区分依据是参数传播深度（扁平 vs 深度嵌套）。

## 关联异常

- **gm_overflow**：共享 Tiling 参数传播链模式，gm_overflow 的 tiling 偏移驱动 GM 越界，tiling_overflow 的 tiling 大小驱动 UB 溢出。
- **ub_capacity_overflow**：共享 UB buffer 容量约束，ub_capacity_overflow 为静态声明超容，tiling_overflow 为动态搬运超容。
- **tiling_ub_misalign**：共享 Tiling 参数 Host→Kernel 传播链，tiling_ub_misalign 传播 `misalignBytes`，tiling_overflow 传播 `copyBytes`。
- **共享参数**：（推断，非 cross_references "参数共享"表收录）与 gm_overflow/tiling_ub_misalign 共享 Tiling 参数传播链；与 ub_capacity_overflow 共享 UB_CAPACITY 约束。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02）、存在性检查（RULE-03/04）、API 调用模式（RULE-DEPTH）
- **命中判定**：
  - 值域判断类规则——在源码定位 `tiling.FIELD = VALUE` 赋值，VALUE 为编译期常量且 > bufferAllocSize 即命中；乘法表达式计算结果与 UINT32_MAX / bufferCapacity 比对；跨行追溯 ≤3 层
  - 存在性检查类规则——全文模式搜索 TilingStruct 声明后是否在首次使用前完成字段赋值；全文搜索 Host 侧计算函数是否存在 `<= UB_CAPACITY` 等范围校验断言（缺失即命中）
  - API 调用模式类规则——统计 DataCopy 调用链深度，识别 ≥2 层函数调用传播的 tiling 参数，验证最终 copyLen 与 bufferAllocSize/sizeof(T) 比对
- **修复映射**：
  - tiling_overflow-RULE-01（buffer size 超大常量）→ 修复模式 1（限制上限）
  - tiling_overflow-RULE-02（乘法溢出）→ 修复模式 2（乘法溢出保护）
  - tiling_overflow-RULE-03（参数未初始化）→ 修复模式 3（初始化与校验）
  - tiling_overflow-RULE-04（Host 侧校验缺失）→ 修复模式 4（Host 侧参数校验）
  - tiling_overflow-RULE-DEPTH（深度嵌套传播）→ 修复模式 1 / 修复模式 4
