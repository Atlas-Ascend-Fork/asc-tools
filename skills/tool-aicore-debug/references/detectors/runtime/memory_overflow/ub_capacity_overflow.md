# ub_capacity_overflow — UB 总分配超容量

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | ub_capacity_overflow |
| 异常类型 | UB总分配超容量 |
| 错误码 | 无（EZ9999 报错无数值型错误码） |
| 类别 | memory_overflow |
| 源文件 | 来源: stable_ez9999 历史种子（legacy: B3） |
| 报错模式 | EZ9999_stderr |
| 内存区域 | UB |
| kernel_diff_type | kernel_diff |
| behavior_note | - |

## 触发条件

> **容量适用范围**：本文容量数值记录历史种子环境，不代表所有芯片或 tensor 的可访问容量。当前输入须先确认架构、API 重载及实际分配边界；未知时标记 `runtime_required=true`。统一单位与计分口径见 `references/constraints/semantic_contract.md`。

AscendC kernel 中通过 `__ubuf__` 声明（TC-Low）或 `pipe.InitBuffer`（TC-High）分配的所有 UB buffer 的总大小超过 AI Core 的 UB 容量限制。在 Ascend 910 系列上，UB 容量为 192KB（196608 字节）。历史种子（legacy: B3） 通过 3 个 `__ubuf__ float buf[131072]` 直接声明触发（3×512KB），seed-B3q 通过 3 个 `pipe.InitBuffer` 的 Queue 驱动方式触发（合计 1600KB）。

- 违规模式（历史种子（legacy: B3））：`__ubuf__ float buf0[131072]` + `buf1[131072]` + `buf2[131072]`，3 × 512KB = 1536KB >> 192KB
- 违规模式（seed-B3q）：`pipe.InitBuffer(qBuf1, 1, 600*1024)` + `qBuf2, 1, 600*1024` + `qBuf3, 1, 400*1024`，3 队列合计 1600KB >> 192KB
- 安全模式：每块/每队列改为 `TILE_LENGTH(256)` / `256 * sizeof(float)`（总计 << 192KB）
- 最小触发条件：单个 kernel 中所有 UB buffer 声明/分配的总大小超过 UB 物理容量 192KB（196608 字节），触发 `mte_ub_wr_ovflw`

## 检测规则

### ub_capacity_overflow-RULE-01：__ubuf__ 声明总大小超限

**【条件】**单个 kernel 函数内所有 `__ubuf__ TYPE NAME[SIZE]` 声明的总大小 `Σ(SIZE_i × sizeof(TYPE_i)) > 196608`（192KB）
**【API】**`__ubuf__` 声明
**【约束】**`Σ(SIZE_i × sizeof(TYPE_i)) <= 196608`
**【来源】**代码 diff 直接提取
**【匹配模式】** 值域判断

匹配示例（KB）：

- 历史种子（legacy: B3） 中 3 × `__ubuf__ float buf[131072]` = 3 × 512KB = 1536KB >> 192KB

匹配示例（detector 补充）：

```cpp
__ubuf__ float buf1[49152];  // 192KB
__ubuf__ float buf2[49152];  // 192KB
__ubuf__ float buf3[49152];  // 192KB
__ubuf__ float buf4[49152];  // 192KB
// 总大小 768KB > 192KB
```

### ub_capacity_overflow-RULE-02：单个 Queue InitBuffer 容量超限

**【条件】**单个 `QUEUE.InitBuffer(COUNT, SIZE)` 中 `COUNT × SIZE > 196608`
**【API】**`pipe.InitBuffer` / TQue
**【约束】**`COUNT × SIZE <= 196608`
**【来源】**代码 diff 直接提取
**【匹配模式】** 值域判断

### ub_capacity_overflow-RULE-03：单个 buffer 超限

**【条件】**单个 `__ubuf__ TYPE NAME[SIZE]` 中 `SIZE × sizeof(TYPE) > 196608`
**【API】**`__ubuf__` 声明
**【约束】**`SIZE × sizeof(TYPE) <= 196608`
**【来源】**API 文档约束
**【匹配模式】** 值域判断

匹配示例（KB）：

- `__ubuf__ float buf[65536]` = 256KB > 192KB

### ub_capacity_overflow-RULE-04：模板参数驱动的 buffer 大小超限

**【条件】**模板参数 TILE_LEN 用于 `__ubuf__ TYPE buf[TILE_LEN]`，某些实例化值使 `TILE_LEN × sizeof(TYPE) > 196608`
**【API】**模板参数 → `__ubuf__` 声明
**【约束】**所有实例化值满足 `P × sizeof(TYPE) <= 196608`
**【来源】**LLM 推断
**【匹配模式】** 值域判断

### ub_capacity_overflow-RULE-05：Tiling 驱动的动态分配超限

**【条件】**`qIn.InitBuffer(tiling->bufCount, tiling->bufSize)` 中 `tiling->bufCount × tiling->bufSize > 196608`
**【API】**Tiling 参数 → `InitBuffer`
**【约束】**`tiling->bufCount × tiling->bufSize <= 196608`
**【来源】**LLM 推断
**【匹配模式】** 变量名匹配

### ub_capacity_overflow-RULE-06：隐式系统开销预留不足

**【条件】**UB 总分配接近 192KB（如 185-192KB 范围），未预留编译器/运行时的系统开销（约 4-10KB），实际可用 UB < 192KB
**【API】**`__ubuf__` / `InitBuffer`
**【约束】**`totalUB <= 184320`（180KB 安全阈值）
**【来源】**API 文档约束
**【匹配模式】** 值域判断

### ub_capacity_overflow-RULE-QUEUE：多 Queue InitBuffer 总和超限（Queue 驱动）

**【条件】**同一 kernel 中多个 `pipe.InitBuffer` 调用的 size 参数之和 `Σ(size_i × count_i) > 196608`（192KB），单个 Queue 的分配可能不超限但总和超限
**【API】**`pipe.InitBuffer` / `TQue`（多个 Queue）
**【约束】**`Σ(COUNT_i × SIZE_i) <= 196608`
**【来源】**运行验证
**【匹配模式】** 存在性检查

匹配示例（KB）：

- seed-B3q 中 `InitBuffer(q1,1,614400) + InitBuffer(q2,1,614400) + InitBuffer(q3,1,409600)` = 1638400 >> 196608
- 覆盖种子：stable_ez9999 历史种子（legacy: B3q）
- 与 ub_capacity_overflow-RULE-02 的区别：ub_capacity_overflow-RULE-02 检查单个 Queue 超限；ub_capacity_overflow-RULE-QUEUE 检查多个 Queue 之和超限（单 Queue 可能合法）

## 修复方案

### 标准修复模式

1. **修复模式 1（减小 buffer 大小）**：减小每个 `__ubuf__` 或 `InitBuffer` 的 SIZE
2. **修复模式 2（Buffer 复用）**：不同处理阶段复用同一 buffer
3. **修复模式 3（Tiling 策略调整）**：减小 tile size 增加循环次数
4. **修复模式 4（Queue 分时复用）**：使用 TQue 机制管理 buffer 生命周期
5. **修复模式 5（混合精度优化）**：中间计算使用 half 精度节省空间

### seed 具体修复

- 历史种子（legacy: B3）：`__ubuf__ float buf[131072]` × 3 → `__ubuf__ float buf[TILE_LENGTH]` × 3（TILE_LENGTH=256，总计 3KB << 192KB）
- seed-B3q：`InitBuffer(qBuf_i, 1, {600,600,400}*1024)` → `InitBuffer(qBuf_i, 1, 256*sizeof(float))`（总计 3KB << 192KB）

### 代码模板

模板 1：UB 安全容量检查（detector 补充）

```cpp
template<typename T, uint32_t... SIZES>
constexpr bool CheckUBCapacity() {
    uint32_t total = 0;
    for (auto s : {SIZES...}) {
        total += s * sizeof(T);
    }
    return total <= 196608;  // 192KB
}

static_assert(CheckUBCapacity<float, 12288, 12288, 12288, 12288>(),
              "UB total allocation exceeds 192KB");
```

模板 2：自适应 Tile 大小（detector 补充）

```cpp
template<typename T>
class UBSafeKernel {
    static constexpr uint32_t UB_CAPACITY = 196608;
    static constexpr uint32_t NUM_BUFFERS = 2;  // 输入 + 输出
    static constexpr uint32_t MAX_TILE_ELEMENTS =
        (UB_CAPACITY / NUM_BUFFERS) / sizeof(T);
    static constexpr uint32_t SAFE_TILE_ELEMENTS = MAX_TILE_ELEMENTS * 9 / 10;
public:
    __aicore__ inline void Init(GM_ADDR x, GM_ADDR y, uint32_t totalLength) {
        xGM.Init(x);
        yGM.Init(y);
        this->totalLength = totalLength;
    }
private:
    __ubuf__ T xBuf[SAFE_TILE_ELEMENTS];
    __ubuf__ T yBuf[SAFE_TILE_ELEMENTS];
    uint32_t totalLength;
};
```

### 防御性编程建议

1. 编译期 `static_assert` 容量检查
2. 安全阈值 180KB（预留 10% 系统开销）
3. Buffer 复用优先于扩大 buffer
4. Tiling 配合减小 tile size 控制总分配
5. 容量速查表（按数据类型快速查询可用元素数）
6. TQue 机制管理 buffer 生命周期

### 容量计算参考表

| 数据类型 | sizeof | 192KB 可容纳元素数 | 180KB 安全阈值 |
|---------|--------|------------------|---------------|
| float | 4B | 49152 | 46080 |
| half | 2B | 98304 | 92160 |
| int8_t | 1B | 196608 | 184320 |
| int32_t | 4B | 49152 | 46080 |

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| 直接 __ubuf__ 声明超容 | stable_ez9999 历史种子（legacy: B3） | 3 × `__ubuf__ float buf[131072]`（TC-Low，3×512KB） | EC3b |
| Queue 驱动超容 | stable_ez9999 历史种子（legacy: B3q） | 3 × `pipe.InitBuffer` 合计 1600KB（TC-High） | EC3c |
| 单 buffer 过大 | 变体（待生成） | 单个 `__ubuf__ float buf[65536]` = 256KB | EC3a |
| 模板实例化放大 | 变体（待生成） | 模板参数导致 buffer 大小膨胀超限 | EC3a |
| Tiling 动态分配超限 | 变体（待生成） | `tiling->bufCount × tiling->bufSize > 196608` | EC3b |
| 条件编译隐藏 | 变体（待生成） | `#ifdef` 分支中声明的额外 buffer 未计入总量 | EC3b |

## 等价类划分

变异维度：UB 总分配大小 TOTAL_UB 相对于 UB 容量 CAPACITY（196608 字节）

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | TOTAL_UB ≤ 184320（180KB） | 0 ~ 184320 | 安全阈值内，预留系统开销 |
| EC2 | 184320 < TOTAL_UB ≤ 196608 | 184321 ~ 196608 | 临界区，可能因系统开销溢出 |
| EC3a | 单个 __ubuf__ > 196608 | SIZE × sizeof(T) > 192KB | 单 buffer 超限，编译期可确定 |
| EC3b | 多 __ubuf__ 总和 > 196608 | Σ > 192KB | 多 buffer 叠加超限 |
| EC3c | 多 Queue InitBuffer 总和 > 196608 | Σ(count×size) > 192KB | Queue 驱动超限，覆盖 seed-B3q |

划分依据：UB 物理容量 192KB（196608 字节）硬限制。EC3a-c 区分依据是分配方式（单 buffer / 多 buffer / Queue）。

## 关联异常

- **gm_overflow**：同属 02_overflow 类别，ub_capacity_overflow 为 UB 区域超容，gm_overflow 为 GM 区域越界。
- **vec_ub_overflow**：UB 地址越界（Vector 操作超出 UB 容量），与 ub_capacity_overflow 共享 UB 容量约束（语义相关，推断；vec_ub_overflow 在 cross_references "症状相似" UB overflow 组中，组内为 {vec_ub_overflow, mte_write_ub_overflow, mte_read_ub_overflow, datacopypad_ub_overflow}，ub_capacity_overflow 不在该组）。
- **mte_write_ub_overflow/mte_read_ub_overflow**：MTE 写/读 UB 溢出，与 ub_capacity_overflow 共享 UB 容量约束（语义相关，推断；mte_write_ub_overflow/mte_read_ub_overflow 在 cross_references "症状相似" UB overflow 组中，ub_capacity_overflow 不在该组）。
- **共享参数**：（推断，非 cross_references "参数共享"表收录）与 mte_write_ub_overflow/mte_read_ub_overflow 共享 UB_CAPACITY=196608 容量约束；与 vec_ub_overflow 共享 UB buffer 分配族。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02/03/04/06）、变量名匹配（RULE-05）、存在性检查（RULE-QUEUE）
- **命中判定**：
  - 值域判断类规则——统计单个 kernel 函数内所有 `__ubuf__ TYPE NAME[SIZE]` 声明，计算 `Σ(SIZE_i × sizeof(TYPE_i))`，与 196608 / 184320 比对；对单个 InitBuffer 调用验证 `COUNT × SIZE <= 196608`；对模板参数需枚举所有实例化值
  - 变量名匹配类规则——在源码定位 `tiling->bufCount` / `tiling->bufSize`，追溯 Host 侧赋值表达式；追溯受限时标记 `trace_limited=true`
  - 存在性检查类规则——全文模式搜索多个 `pipe.InitBuffer` 调用，验证是否存在 `Σ(COUNT_i × SIZE_i) <= 196608` 的总和校验
- **修复映射**：
  - ub_capacity_overflow-RULE-01（多 __ubuf__ 总和超限）→ 修复模式 1（减小 buffer）/ 修复模式 2（复用）
  - ub_capacity_overflow-RULE-02/03（单 Queue / 单 buffer 超限）→ 修复模式 1 / 修复模式 3
  - ub_capacity_overflow-RULE-04（模板参数超限）→ 修复模式 1 / 修复模式 3
  - ub_capacity_overflow-RULE-05（Tiling 动态分配超限）→ 修复模式 1 / 修复模式 3
  - ub_capacity_overflow-RULE-06（系统开销预留不足）→ 修复模式 1（按 180KB 阈值）
  - ub_capacity_overflow-RULE-QUEUE（多 Queue 总和超限）→ 修复模式 4（Queue 分时复用）/ 修复模式 2
