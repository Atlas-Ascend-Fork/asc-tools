# fixpipe_srcstride_overflow — FixPipe srcStride 读方向溢出

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | fixpipe_srcstride_overflow |
| 异常类型 | fixp_err_read_l1_ovflw（srcStride 读方向溢出；对偶于 fixpipe_l1_overflow，方向与 fixpipe_gm_overflow/atomic_fixpipe_overflow 的 fixp_err_out_write_overflow 相反） |
| 错误码 | errcode:(0, 0x4000000000, 0) The read address of the L0C is out of range（subErrType=4） |
| 类别 | fixpipe |
| 源文件 | cann-aicore/new_case/fixpipe_addr_misalign/error/main.asc:82（`fp.srcStride = m * 100`） |
| 报错模式 | EZ9999_stderr（retCode=0x26 [aicore exception]，aic error mask=0x6500020bd00028c，errcode:(0, 0x4000000000, 0)，subErrType=4） |
| 内存区域 | L0C/CO1（FixPipe 读源，srcStride 读方向溢出，对偶于 fixpipe_l1_overflow） |
| kernel_diff_type | kernel_diff |
| kernel_diff_note | kernel_diff（srcStride 常量注入 m*100 → m） |
| behavior_note | 深度链种子；完整 Cube 流水线（Nd2Nz→LoadData2D→Mmad→Fixpipe），Fixpipe 阶段 fp.srcStride = m*100（3200）远超 CO1 1024-float buffer，单块读址越界；fixed 版 srcStride = m（32） |

## 触发条件

在 FixPipe 操作中，`srcStride` 被设置为过大的值，导致从 L0C/CO1 缓冲区读取数据时地址越界（读方向溢出，触发 fixp_err_read_l1_ovflw）。完整 Cube 计算流水线（Nd2Nz→LoadData2D→Mmad→Fixpipe）在 Fixpipe 阶段通过 `fp.srcStride = m * 100`（m=32, srcStride=3200）使 FixPipe 从 L0C/CO1 读取时按 srcStride=3200 步进，远超 CO1 的 1024-float 缓冲区 → L0C 读越界（errcode:(0, 0x4000000000, 0)，subErrType=4）。与 fixpipe_l1_overflow（同为 fixp_err_read_l1_ovflw 读方向溢出）对偶；与 fixpipe_gm_overflow / atomic_fixpipe_overflow（fixp_err_out_write_overflow 写方向溢出）方向相反。

- 违规模式（error 版）：`fp.srcStride = m * 100`（m=32, srcStride=3200）— 单块 Fixpipe 读址 [0, 3200) floats 跨越 CO1 1024-float 边界 → L0C 读越界
- 安全模式（fixed 版）：`fp.srcStride = m`（即 32）— 合理 stride 值，单块读址 [0, 32) floats 全在 CO1 buffer 内
- 最小触发条件：Fixpipe 的 srcStride 参数过大（srcStride > CO1_SIZE_FLOATS = 1024），单块读址越界

来源标注：触发场景取自 `references/indices/seed_mapping.md:23` behavior_note（"深度链种子，FixPipe srcStride 溢出；基于 fixpipe_gm_overflow 改造，srcStride 读方向溢出"）+ `new_case/fixpipe_addr_misalign/README.md` 实测描述。种子路径 `fixpipe_addr_misalign`（需求名"地址非对齐"，唯一已验证的 FixPipe 触发路径为 srcStride 致 L0C 读越界，属地址类异常）。

## 检测规则

### fixpipe_srcstride_overflow-RULE-01：FixpipeParamsV220.srcStride 放大致 L0C 读越界

**【条件】**代码中出现 `fp.srcStride = EXPR * CONSTANT`（如 `fp.srcStride = m * 100`），srcStride 由乘法计算且无 `min` / `ASSERT` 保护，单块读址 `srcStride > CO1_SIZE_FLOATS`（dav-2201 CO1 buffer = 1024 floats）
**【API】**Fixpipe / FixpipeParamsV220.srcStride 赋值
**【约束】**`fp.srcStride <= CO1_SIZE_FLOATS`（1024）；srcStride 应等于 m（Mmad 输出行 stride），禁止人为放大
**【匹配模式】** 值域判断

匹配示例（error 版 main.asc:82）：

```cpp
fp.srcStride = m * 100;   // m=32 → srcStride=3200，单块读址越界
```

### fixpipe_srcstride_overflow-RULE-02：srcStride × ndNum 累积读越界

**【条件】**`srcStride` 与 `ndNum` 同时设置但未校验乘积，`srcStride × ndNum` 超出 CO1 实际布局；或 `srcNdStride` 累积偏移跨越 CO1 边界
**【API】**Fixpipe（srcStride × ndNum / srcNdStride 组合）
**【约束】**`static_cast<uint64_t>(srcStride) × ndNum <= CO1_SIZE_FLOATS`（1024）；`srcNdStride` 累积偏移不跨 CO1 边界
**【匹配模式】** 值域判断

匹配示例（detector 补充）：

```cpp
fp.srcStride = tileM * sizeof(float);
fp.ndNum = totalIterations;
fp.srcNdStride = largeOffset;
```

### fixpipe_srcstride_overflow-RULE-03：读址范围校验缺失

**【条件】**Fixpipe 调用前缺少 `srcStride <= CO1_SIZE_FLOATS` / `srcStride × ndNum <= CO1_SIZE_FLOATS` 的断言或判断
**【API】**Fixpipe（调用前范围校验）
**【约束】**Fixpipe 调用前存在 `ASSERT(fp.srcStride <= CO1_SIZE_FLOATS)` 或等价范围校验
**【匹配模式】** 存在性检查

### fixpipe_srcstride_overflow-RULE-04：读方向 vs 写方向异常区分

**【条件】**Fixpipe 报错 stderr 含 `errcode:(0, 0x4000000000, 0) The read address of the L0C is out of range` / `subErrType=4` / `fixp_err_read_l1_ovflw`，且代码侧 srcStride 异常（区分于 dstStride 驱动的 fixp_err_out_write_overflow 写方向溢出）
**【API】**Fixpipe（读方向 srcStride vs 写方向 dstStride）
**【约束】**stderr 错误码 0x4000000000（读越界）对应 srcStride 异常；dstStride 异常对应写方向溢出（fixpipe_gm_overflow / atomic_fixpipe_overflow，错误码 141）
**【匹配模式】** 存在性检查

匹配示例（stderr_sample.txt:4）：

```text
errcode:(0, 0x4000000000, 0) errorStr: The read address of the L0C is out of range. ... subErrType:4.
```

## 修复方案

### 标准修复模式

1. **修复模式 1（srcStride 恢复为合法值 m）**：`fp.srcStride = m;`（即 32），与 Mmad 输出矩阵行 stride 一致，单块读址 [0, m) floats 全在 CO1 buffer 内
2. **修复模式 2（srcStride 边界限制）**：`uint32_t maxStride = CO1_SIZE_FLOATS / std::max(fp.ndNum, 1u); fp.srcStride = std::min(rawStride, maxStride); ASSERT(static_cast<uint64_t>(fp.srcStride) * fp.ndNum <= CO1_SIZE_FLOATS);`
3. **修复模式 3（读址范围校验）**：Fixpipe 调用前插入 `ASSERT(fp.srcStride <= CO1_SIZE_FLOATS)` 守卫

### seed 具体修复

- error main.asc:82 `fp.srcStride = m * 100;`（srcStride=3200） → fixed main.asc:82 `fp.srcStride = m;`（srcStride=32）
- error main.asc:135 `return 1;` → fixed main.asc:135 `return 0;`（修复后正常退出）

### 代码模板

模板 1：安全 Fixpipe srcStride 计算（detector 补充）

```cpp
constexpr uint32_t CO1_SIZE_FLOATS = 1024;   // dav-2201 CO1 buffer 容量
inline uint32_t SafeFixpipeSrcStride(uint32_t rawStride, uint32_t ndNum) {
    uint32_t maxStride = ndNum > 0 ? CO1_SIZE_FLOATS / ndNum : CO1_SIZE_FLOATS;
    uint32_t safe = std::min(rawStride, maxStride);
    ASSERT(static_cast<uint64_t>(safe) * ndNum <= CO1_SIZE_FLOATS);
    return safe;
}
```

模板 2：seed 直接修复（m*100 → m）

```cpp
// 修复前（error）
fp.srcStride = m * 100;   // 3200，越界

// 修复后（fixed）
fp.srcStride = m;         // 32，匹配 Mmad 输出行 stride
```

### 防御性编程建议

1. **srcStride 与 Mmad 输出布局一致**：srcStride 应等于 m（Mmad 输出行 stride），禁止人为放大或乘以常量
2. **设置 srcStride 前断言**：`ASSERT(fp.srcStride <= CO1_SIZE_FLOATS)`（1024），避免单块读址越界
3. **区分读/写方向溢出**：srcStride 异常触发读方向（fixp_err_read_l1_ovflw，0x4000000000），dstStride 异常触发写方向（fixp_err_out_write_overflow，141），诊断时按 stderr 错误码定向
4. **srcStride × ndNum 64 位校验**：累积读址范围使用 `uint64_t` 计算，避免 32 位溢出误判
5. **tiling 参数校验**：在 tiling 模板中校验 srcStride × ndNum 不超过 CO1 容量
6. **日志记录 srcStride**：在 Fixpipe 关键路径记录 srcStride、ndNum、m 值，便于 L0C 读越界快速定位

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| srcStride 常量倍数放大（基准） | cann-aicore/new_case/fixpipe_addr_misalign | `fp.srcStride = m * 100`（srcStride=3200）单块读址越界 | EC2 |
| srcStride × ndNum 累积越界 | 变体（待生成） | srcStride 与 ndNum 乘积超出 CO1 | EC2 |
| srcNdStride 累积偏移越界 | 变体（待生成） | srcNdStride 跨 CO1 边界累积 | EC2 |
| 写方向对偶（dstStride 驱动） | cann-aicore/10_fixpipe/fixpipe_write_oob_verified | dstStride 驱动 GM 写溢出（见 fixpipe_gm_overflow） | EC2（写方向） |

## 等价类划分

变异维度：FixPipe srcStride 相对于 CO1/L0C buffer 容量，以及读/写方向

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | srcStride ≤ CO1_SIZE_FLOATS（1024）且 srcStride × ndNum ≤ CO1_SIZE_FLOATS | 合法范围 | 读址范围在 CO1 内，安全（fixed 版 srcStride=32） |
| EC2 | srcStride 单块越界或 srcStride × ndNum 累积越界 | srcStride > 1024 或乘积 > 1024 | 编译期可确定读越界，覆盖原始种子（error 版 srcStride=3200） |

划分依据：FixPipe 对 L0C/CO1 读地址的范围检查。EC1 为合法读址范围，EC2 为编译期可确定的读越界。读方向（srcStride，fixp_err_read_l1_ovflw）与写方向（dstStride，fixp_err_out_write_overflow，见 fixpipe_gm_overflow）方向对偶，分属不同异常类型。

## 关联异常

> 与 T2/T3 同口径，仅取自 references/indices/ 反查（标注反查源）；推断性内容前置标注。

- **srcStride（参数共享）**：cross_references 反查：`srcStride → [fixpipe_l1_overflow, fixpipe_srcstride_overflow]`（参数共享，cross_references.md:15，约束 `srcStride × 计算单元 ≤ L1 大小`）。fixpipe_srcstride_overflow 与 fixpipe_l1_overflow 共享 srcStride 参数溢出机制；区分：fixpipe_srcstride_overflow 种子 srcStride=3200 单块读址越界 CO1（1024 floats，errcode 0x4000000000），fixpipe_l1_overflow 种子 srcStride=3200 触发 L1 读越界（错误码 103）。
- **FixPipe / fixp_err_read_l1_ovflw / srcStride（共享 keyword）**：keyword_index 反查：`FixPipe → [fixpipe_gm_overflow, fixpipe_cfg_illegal, fixpipe_l1_overflow, atomic_fixpipe_overflow, fixpipe_srcstride_overflow]`（异常名称）、`fixp_err_read_l1_ovflw → [fixpipe_l1_overflow, fixpipe_srcstride_overflow]`（报错描述）、`srcStride → [fixpipe_srcstride_overflow]`（触发条件）。
- **（推断，非 cross_references 收录）fixpipe_l1_overflow / fixpipe_gm_overflow**：对称/对偶异常 —— seed_mapping.md:23 声明 fixpipe_srcstride_overflow "基于 fixpipe_gm_overflow 改造，srcStride（读方向）溢出"；与 fixpipe_l1_overflow 同属 fixp_err_read_l1_ovflw（读方向），与 fixpipe_gm_overflow/atomic_fixpipe_overflow 的 fixp_err_out_write_overflow（写方向）方向相反。
- **（推断，非 cross_references 收录）fixpipe_addr_misalign**：种子同源 —— fixpipe_srcstride_overflow 的种子数据取自 `new_case/fixpipe_addr_misalign`，二者共享 srcStride=3200 读越界路径（`references/detectors/runtime/fixpipe/fixpipe_addr_misalign.md` 已收录该 seed 的独立 detector）；fixpipe_srcstride_overflow 为深度链种子视角，强调 srcStride 读方向溢出机制与 fixpipe_gm_overflow 的对偶关系。

## 检测执行指引

- **适用匹配模式**：值域判断（RULE-01/02）、存在性检查（RULE-03/04）
- **命中判定**：
  - 值域判断类规则——提取 `fp.srcStride = EXPR` 表达式，EXPR 含乘法（如 `m * 100`）且无 `min`/`ASSERT` 保护即标记风险；进一步计算 `srcStride` 与 `CO1_SIZE_FLOATS`（1024）比对，`srcStride > 1024` 或 `srcStride × ndNum > 1024` 即命中；跨行追溯 ≤3 层
  - 存在性检查类规则——全文模式搜索 Fixpipe 调用前是否存在 `srcStride <= CO1_SIZE_FLOATS` 断言（缺失即命中 RULE-03）；stderr 含 `errcode:(0, 0x4000000000, 0)` / `subErrType=4` / `The read address of the L0C is out of range` 且代码侧 srcStride 异常即命中 RULE-04（区分写方向 dstStride 异常）
- **修复映射**：
  - fixpipe_srcstride_overflow-RULE-01（srcStride 放大致 L0C 读越界）→ 修复模式 1（srcStride 恢复为 m）/ 修复模式 2（边界限制）
  - fixpipe_srcstride_overflow-RULE-02（srcStride × ndNum 累积越界）→ 修复模式 2（srcStride 边界限制）
  - fixpipe_srcstride_overflow-RULE-03（读址范围校验缺失）→ 修复模式 3（插入 ASSERT 守卫）
  - fixpipe_srcstride_overflow-RULE-04（读方向 vs 写方向区分）→ 修复模式 1（srcStride 侧修复；若 stderr 为写方向 141 则转 fixpipe_gm_overflow）
