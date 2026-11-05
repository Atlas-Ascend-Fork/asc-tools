# gm_overflow — GM 地址越界

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | gm_overflow |
| 异常类型 | GM读越界 |
| 错误码 | 无（EZ9999 报错无数值型错误码） |
| 类别 | memory_overflow |
| 源文件 | 来源: stable_ez9999 历史种子（legacy: B2） |
| 报错模式 | EZ9999_stderr |
| 内存区域 | GM |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

GM（Global Memory）指针偏移超出 `aclrtMalloc` 分配的内存范围。NPU AI Core 的 MTE（Memory Transfer Engine）在执行 DMA 搬运时对访问地址进行范围检查。当源地址或目标地址超出实际分配区域时，硬件触发 `mte access out of range` 异常。历史种子（legacy: B2） 通过 `x + 99999999` 触发读越界，seed-B2w 通过 `z + 99999999` 触发写越界，seed-B2t 通过 tiling 参数 `offsetElements=99999999` 驱动越界。

- 违规模式（历史种子（legacy: B2））：`(float*)(xGM_base + 99999999)` — GM 读越界，偏移 99999999 远超 aclrtMalloc 分配范围
- 违规模式（seed-B2w）：`z + 99999999` 作为 `asc_copy_ub2gm_sync` 目标 — GM 写越界
- 违规模式（seed-B2t）：`xGM[offsetElements]` 其中 `offsetElements = 99999999` 来自 tiling — GM 越界（Tiling 驱动）
- 安全模式：使用合法偏移（`x`、`z` 或 `offsetElements = 0`）
- 最小触发条件：GM 指针偏移超出 `aclrtMalloc` 分配范围，MTE 执行 DMA 搬运时触发 `mte access out of range`

## 检测规则

### gm_overflow-RULE-01：超大常量偏移（GM 读越界）

**【条件】**代码中出现 `(TYPE*)(GM_PTR + LARGE_CONSTANT)` 或 `GM_PTR[LARGE_INDEX]`，偏移常量值超过合理范围（如 > 1MB 且无对应分配）
**【API】**GM 指针算术 / 数组索引
**【约束】**`offset + accessLength <= allocSize`
**【来源】**代码 diff 直接提取
**【匹配模式】** 值域判断

匹配示例（KB）：

- `x + 99999999` — 偏移 99999999 字节远超 1024 字节分配

匹配示例（detector 补充）：

```cpp
float* xGM = (float*)(xGM_base + 99999999);
DataCopy(dst, xGM + 99999999, params);
```

### gm_overflow-RULE-02：DataCopy 参数范围越界

**【条件】**`DataCopy(DST, SRC, PARAMS)` 中 `PARAMS.blockLen × PARAMS.blockCount × sizeof(T)` 超出分配，或 `srcStride / dstStride` 导致地址跳跃越界
**【API】**DataCopy
**【约束】**`blockLen × blockCount × sizeof(T) <= bufferAllocSize`
**【来源】**API 文档约束
**【匹配模式】** 值域判断

### gm_overflow-RULE-03：Tiling 驱动的 GM 偏移表达式（通用）

**【条件】**代码中出现 `GM_PTR + tiling->FIELD` 或 `GM_PTR[tiling->FIELD]`，tiling 偏移字段可能超出合理范围，缺少 `offset + length <= totalSize` 边界检查
**【API】**Tiling 参数 → GM 地址计算
**【约束】**`tiling.offset + tiling.accessLength <= tiling.totalAllocSize`
**【来源】**LLM 推断
**【匹配模式】** API 调用模式

### gm_overflow-RULE-04：尾块偏移校验缺失

**【条件】**循环 tiling 中 `(tileCount - 1) * tileLength + tileLength > totalLength`，缺少 `min(remain, tileLength)` 尾块保护
**【API】**循环中的 DataCopy / GM 指针算术
**【约束】**`last_offset + tileLength <= totalLength`
**【来源】**LLM 推断
**【匹配模式】** 值域判断

### gm_overflow-RULE-05：多维索引计算越界

**【条件】**`GM_PTR[row * STRIDE + col]` 中 `row * STRIDE + col` 超出 totalSize，STRIDE 与实际分配维度不匹配
**【API】**GM 多维索引
**【约束】**`row * STRIDE + col < totalSize`
**【来源】**LLM 推断
**【匹配模式】** 值域判断

### gm_overflow-RULE-06：类型转换后字节偏移越界

**【条件】**`(TYPE*)(BYTE_PTR + BYTE_OFFSET)` 中 BYTE_OFFSET 以字节为单位但超出分配，与按元素的分配大小不一致
**【API】**类型转换 + 指针算术
**【约束】**`BYTE_OFFSET + accessSize <= allocSize`
**【来源】**API 文档约束
**【匹配模式】** 值域判断

### gm_overflow-RULE-WRITE：GM 写越界（常量偏移）

**【条件】**`asc_copy_ub2gm_sync` 或其他 UB→GM 搬运 API 的 `__gm__` 目标指针含常量加法偏移（如 `z + LARGE_CONSTANT`），使写入地址超出 `aclrtMalloc` 分配范围
**【API】**`asc_copy_ub2gm_sync` / DataCopy（UB→GM 方向）
**【约束】**`writeAddress + writeLength <= gmAllocEnd`
**【来源】**运行验证
**【匹配模式】** API 调用模式

匹配示例（KB）：

- seed-B2w 中 `__gm__ float* badDest = z + 99999999` 作为 `asc_copy_ub2gm_sync` 目标
- 覆盖种子：stable_ez9999 历史种子（legacy: B2w）

### gm_overflow-RULE-TILING：GM 越界（Tiling 驱动）

**【条件】**kernel 中 `xGM[offsetElements]` 其中 offsetElements 来自 tiling 参数结构体（`tiling->offsetElements`），经 Host → GM → Kernel 传播链传入，参数值超出 GM 分配范围
**【API】**`xGM[index]` GM 数组索引（index 来自 tiling 参数）
**【约束】**`offsetElements + accessLength <= totalElements`
**【来源】**运行验证
**【匹配模式】** 变量名匹配

匹配示例（KB）：

- seed-B2t 中 `DataCopy(xLocal, xGM[offsetElements], totalLength)` 其中 `offsetElements = 99999999`
- 覆盖种子：stable_ez9999 历史种子（legacy: B2t）

## 修复方案

### 标准修复模式

1. **修复模式 1（尾块安全长度）**：`safeLen = (remain < tileLength) ? remain : tileLength`
2. **修复模式 2（Tiling 参数边界校验）**：`tileSize = min(tileSize, totalAllocSize - offset)`
3. **修复模式 3（消除硬编码大偏移）**：使用合法偏移 `xGM_base` 或基于实际分配计算的安全偏移
4. **修复模式 4（DataCopy 参数校验）**：`blockLen × blockCount × sizeof(T) <= bufferAllocSize`
5. **修复模式 5（Host 侧预校验）**：在 Host 计算 tiling 参数时添加 `offset + length <= allocSize` 断言

### seed 具体修复

- 历史种子（legacy: B2）：`(float*)(xGM_base + 99999999)` → `(float*)xGM_base`（即 `x`）
- seed-B2w：`badDest = z + 99999999` → `badDest = z`
- seed-B2t：`B2TilingData tiling = {256, 99999999}` → `B2TilingData tiling = {256, 0}`

### 代码模板

模板 1：安全的 GM 数据搬运（detector 补充）

```cpp
template<typename T>
class SafeGMKernel {
public:
    __aicore__ inline void Init(GM_ADDR x, GM_ADDR y, uint32_t totalLength) {
        xGM.Init(x);
        yGM.Init(y);
        this->totalLength = totalLength;
        tileLength = TILE_LENGTH;
        tileCount = (totalLength + tileLength - 1) / tileLength;
    }

    __aicore__ inline void Process() {
        for (int32_t i = 0; i < tileCount; i++) {
            uint32_t offset = i * tileLength;
            uint32_t remain = totalLength - offset;
            uint32_t safeLen = (remain < tileLength) ? remain : tileLength;
            DataCopy(xLocal, xGM[offset], safeLen);
            DataCopy(yGM[offset], yLocal, safeLen);
        }
    }
private:
    uint32_t totalLength;
    uint32_t tileLength;
    uint32_t tileCount;
};
```

模板 2：GM 偏移安全辅助函数（detector 补充）

```cpp
__aicore__ inline uint32_t SafeAccessLength(
    uint32_t offset, uint32_t requestedLen, uint32_t totalSize) {
    if (offset >= totalSize) return 0;
    uint32_t remain = totalSize - offset;
    return (requestedLen < remain) ? requestedLen : remain;
}
```

### 防御性编程建议

1. 始终计算尾块安全长度 `safeLen = min(remain, tileLength)`
2. Host 侧 tiling 参数断言 `offset + length <= allocSize`
3. 禁止 GM 指针算术中的硬编码大数字
4. 统一单位（字节/元素），避免混用导致计算错误
5. DataCopy 参数审查，验证 blockLen/blockCount 乘积不超限
6. 边界测试（整除/非整除两种场景）

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| GM 读越界（常量偏移） | stable_ez9999 历史种子（legacy: B2） | `(float*)(x + 99999999)`，GM 读 OOB | EC2a |
| GM 写越界（常量偏移） | stable_ez9999 历史种子（legacy: B2w） | `z + 99999999` 作为 UB→GM 写目标 | EC2a |
| GM 越界（Tiling 驱动） | stable_ez9999 历史种子（legacy: B2t） | `xGM[offsetElements]`，offsetElements=99999999 来自 tiling | EC2b |
| DataCopy 参数越界 | 变体（待生成） | `blockLen × blockCount × sizeof(T) > bufferAllocSize` | EC2c |
| 尾块越界 | 变体（待生成） | 循环 tiling 中 `(tileCount-1) × tileLength + tileLength > totalLength` | EC2c |
| 多维索引越界 | 变体（待生成） | `xGM[row * STRIDE + col]` 中 stride/col 计算超出范围 | EC2c |
| 类型转换放大越界 | 变体（待生成） | `(float*)(base + byteOffset)` 中 byteOffset 以字节为单位但超出范围 | EC2a |

## 等价类划分

变异维度：GM 指针偏移量 OFFSET 相对于分配大小 ALLOC_SIZE

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | OFFSET + accessLength ≤ ALLOC_SIZE | 0 ≤ OFFSET ≤ ALLOC_SIZE - accessLength | 访问范围在分配内，安全 |
| EC2a | OFFSET 为编译期常量大值 | 如 99999999 | 编译期可确定越界，覆盖 历史种子（legacy: B2）/B2w |
| EC2b | OFFSET 来自 tiling 参数 | tiling 驱动的运行时值 | 需 Host/Kernel 联合分析，覆盖 seed-B2t |
| EC2c | 循环尾块 OFFSET 超界 | (tileCount-1) × tileLength | 缺少尾块保护导致，覆盖 gm_overflow-RULE-04 |

划分依据：MTE 对 GM 访问地址的范围检查。EC2a-c 的区分依据是偏移来源（常量/tiling/循环）。

## 关联异常

- **tiling_overflow**：共享 Tiling 参数传播链模式，gm_overflow 的 tiling 偏移驱动 GM 越界，tiling_overflow 的 tiling 大小驱动 UB 溢出。
- **ub_capacity_overflow**：同属 02_overflow 类别，gm_overflow 为 GM 区域越界，ub_capacity_overflow 为 UB 区域超容。
- **l2_write_overflow**：共享"地址越界"症状模式（GM/L2 overflow 组），gm_overflow 为 GM 地址越界（读/写），l2_write_overflow 为 L2 地址越界（写）。
- **共享参数**：（推断，非 cross_references "参数共享"表收录）与 datacopy_burst_len_illegal/datacopy_burst_num_illegal 共享 DataCopy blockLen/blockCount 参数族（间接）；与 tiling_overflow 共享 tiling 参数 Host→Kernel 传播链。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02/04/05/06）、API 调用模式（RULE-03/WRITE）、变量名匹配（RULE-TILING）
- **命中判定**：
  - 值域判断类规则——提取 `(TYPE*)(GM_PTR + OFFSET)` 或 `GM_PTR[index]` 表达式并解析 OFFSET；OFFSET 为编译期常量且 > allocSize 即命中；DataCopy 场景读 blockLen × blockCount × sizeof(T) 与 bufferAllocSize 比对；跨行追溯 ≤3 层
  - API 调用模式类规则——统计 `asc_copy_ub2gm_sync`/DataCopy 调用，验证目标指针是否含常量加法偏移；统计 `GM_PTR + tiling->FIELD` 表达式，验证是否存在边界检查
  - 变量名匹配类规则——在源码定位 `offsetElements`/`tiling->offset` 等变量名，追溯 Host 侧赋值表达式；追溯受限时标记 `trace_limited=true`
- **修复映射**：
  - gm_overflow-RULE-01（超大常量偏移）→ 修复模式 3（消除硬编码大偏移）
  - gm_overflow-RULE-02（DataCopy 参数越界）→ 修复模式 4（DataCopy 参数校验）
  - gm_overflow-RULE-03（Tiling 驱动通用）/ gm_overflow-RULE-TILING → 修复模式 2（Tiling 边界校验）/ 修复模式 5（Host 侧预校验）
  - gm_overflow-RULE-04（尾块越界）→ 修复模式 1（尾块安全长度）
  - gm_overflow-RULE-05（多维索引越界）→ 修复模式 3 / 修复模式 4
  - gm_overflow-RULE-06（类型转换越界）→ 修复模式 3 / 修复模式 4
  - gm_overflow-RULE-WRITE（GM 写越界）→ 修复模式 3 / 修复模式 5
