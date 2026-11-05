# l0a_rdwr_conflict — L0A 读写冲突

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | l0a_rdwr_conflict |
| 异常类型 | cube_l0a_rdwr_cflt（L0A 读写冲突） |
| 错误码 | 无（EZ9999 无数值型码；errcode / cube error info 字段 TBD 待运行确认） |
| 类别 | cube |
| 源文件 | cann-aicore/new_case/l0a_rdwr_conflict/error/main.asc |
| 报错模式 | EZ9999_stderr（retCode=0x26 [aicore exception]；cube error info / errcode 字段 TBD 待运行确认） |
| 内存区域 | L0_A |
| kernel_diff_type | kernel_diff |
| behavior_note | TC-Low 平坦 __cube__ 路径；asc_mmad 启动 PIPE_M 长读 L0A 后无 asc_sync() 立即 asc_copy_l12l0a 重写 L0A → L53；cBad 偏移 32640 跨 L0C 128KB 边界 → L13 兜底双路径 |

## 触发条件

> **容量适用范围**：本文容量数值记录历史种子环境，不代表所有芯片或 tensor 的可访问容量。当前输入须先确认架构、API 重载及实际分配边界；未知时标记 `runtime_required=true`。统一单位与计分口径见 `references/constraints/semantic_contract.md`。

Cube 单元的 L0A buffer 在同一时刻被 `asc_copy_l12l0a`（MTE1 写）和 `asc_mmad`（PIPE_M 读）访问，导致读写冲突。TC-Low 平坦 `__cube__` 函数流水线：`asc_copy_gm2l1_nd2nz`（GM→L1）→ `asc_copy_l12l0a`（MTE1 写 L0A）→ `asc_mmad`（PIPE_M/CUBE 读 L0A 写 L0C）。K=256 使 repeatK=16，第一次 `asc_mmad` 需沿 K 维进行 16 轮 L0A/L0B 读取，PIPE_M 读窗口长达数十 cycle；紧接无 `asc_sync()` 立即第二次 `asc_copy_l12l0a` 重写 L0A，MTE1 写与 PIPE_M 读 L0A 时间重叠 → L0A 读写冲突。

- 违规模式（error 版）：`asc_mmad → asc_copy_l12l0a`（同一 aL0a buffer，中间无 `asc_sync()`）→ MTE1 写 L0A 与未完成的 PIPE_M 读 L0A 时间重叠
- 安全模式（fixed 版）：`asc_mmad → asc_sync() → asc_copy_l12l0a`（全管线屏障等待 PIPE_M 读 L0A 完成后再写）
- 最小触发条件：PIPE_M 长读 L0A（K 维 repeatK 轮）尚未完成时，MTE1 通过 `asc_copy_l12l0a` 重写同一 L0A buffer，且两者之间无 `asc_sync()` 屏障
- 次要注入：error 版 `cBad = cL0c + 32640` 使第二个 `asc_mmad` 写址 [32640, 32896) floats 跨越 32768（128KB）L0C 边界 → L13 兜底，双路径保证 EZ9999

来源标注：触发场景取自 `references/indices/seed_mapping.md:22` behavior_note（"深度链种子，L0A 冲突（多一次 LoadData 调用）；基于 l0b_rdwr_conflict 对称改造"）+ `new_case/l0a_rdwr_conflict/README.md` 实测描述（实际 API 为 `asc_copy_l12l0a` / `asc_mmad` / `asc_sync`，TC-Low 平坦路径；seed_mapping "多一次 LoadData 调用" 对应到 `asc_copy_l12l0a` 的 MTE1 写 L0A 语义）。

## 检测规则

### l0a_rdwr_conflict-RULE-01：asc_mmad 后无 asc_sync() 即重写 L0A

**【条件】**`asc_mmad(dst, aL0a, ...)` 启动 PIPE_M 长读 L0A 后，紧接 `asc_copy_l12l0a(aL0a, ...)` 重写同一 L0A buffer，两次调用之间无 `asc_sync()` / `pipe_barrier(PIPE_ALL)` / PIPE_M 定向同步屏障
**【API】**asc_mmad（PIPE_M 读 L0A）/ asc_copy_l12l0a（MTE1 写 L0A）/ asc_sync（屏障）
**【约束】**`asc_mmad` 读 L0A 与后续 `asc_copy_l12l0a` 写同一 L0A 之间存在 `asc_sync()`（全管线屏障）或针对 PIPE_M 的定向屏障
**【匹配模式】** API 调用模式

匹配示例（error 版 main.asc:30-32）：

```cpp
asc_mmad(cL0c, aL0a, bL0b, M, N, K, 0, false, false, false);   // PIPE_M 长读 L0A（K=256, repeatK=16）
asc_copy_l12l0a(aL0a, aL1, 0, (uint8_t)repeatK, (uint16_t)repeatM, 0);   // 【无 asc_sync()】MTE1 重写 L0A → 冲突
```

### l0a_rdwr_conflict-RULE-02：连续 asc_copy_l12l0a 写 L0A 无同步

**【条件】**连续两次 `asc_copy_l12l0a(aL0a, ...)` 写同一 L0A buffer，中间穿插 `asc_mmad` 读 L0A 但全程无 `asc_sync()` 屏障；MTE1 第二次写与 PIPE_M 读 / 第一次写时间重叠
**【API】**asc_copy_l12l0a（连续写 L0A）/ asc_mmad（穿插读 L0A）/ asc_sync（屏障）
**【约束】**连续两次 `asc_copy_l12l0a` 写同一 L0A 之间存在 `asc_sync()` 屏障，且与穿插的 `asc_mmad` 读 L0A 之间亦存在屏障
**【匹配模式】** API 调用模式

匹配示例（error 版 main.asc:28-34，全程无 asc_sync()）：

```cpp
asc_copy_l12l0a(aL0a, aL1, 0, (uint8_t)repeatK, (uint16_t)repeatM, 0);   // 第一次 MTE1 写 L0A
asc_copy_l12l0b(bL0b, bL1, 0, (uint8_t)repeatN, (uint16_t)repeatK, 0);
asc_mmad(cL0c, aL0a, bL0b, M, N, K, 0, false, false, false);            // PIPE_M 读 L0A
asc_copy_l12l0a(aL0a, aL1, 0, (uint8_t)repeatK, (uint16_t)repeatM, 0);   // 第二次 MTE1 写 L0A → 冲突
```

### l0a_rdwr_conflict-RULE-03：K 维放大 PIPE_M 长读窗口重叠

**【条件】**`asc_mmad` 的 K 参数 > 16（使 repeatK = (K+15)/16 ≥ 2），PIPE_M 读 L0A 窗口长达数十 cycle；此时同一 `__cube__` 函数内无 `asc_sync()` 即触发 `asc_copy_l12l0a` 重写 L0A，MTE1 写窗口落入 PIPE_M 读窗口
**【API】**asc_mmad（K 参数驱动 PIPE_M 读窗口长度）/ asc_copy_l12l0a
**【约束】**当 `asc_mmad` 的 K > 16 时，后续对同一 L0A 的 `asc_copy_l12l0a` 必须先经 `asc_sync()` 等待 PIPE_M 长读完成
**【匹配模式】** 值域判断

匹配示例（error 版 main.asc:7，K=256 → repeatK=16，PIPE_M 读窗口被放大）：

```cpp
constexpr uint32_t M = 16, N = 16, K = 256;
```

## 修复方案

### 标准修复模式

1. **修复模式 1（asc_mmad 与 asc_copy_l12l0a 间插入 asc_sync()）**：在第一次 `asc_mmad`（PIPE_M 读 L0A）与第二次 `asc_copy_l12l0a`（MTE1 写 L0A）之间插入 `asc_sync();`（等价 `pipe_barrier(PIPE_ALL)`），等待 PIPE_M 长读 L0A 完成后再写
2. **修复模式 2（移除跨 L0C 边界的次要注入）**：将 `cBad = cL0c + C_BAD_OFFSET` 改写回正常地址 `cL0c`，避免 L13 兜底路径干扰 L53 主诊断
3. **修复模式 3（定向 PIPE_M 屏障替代全管线屏障）**：若仅需等待 PIPE_M，可用 `pipe_barrier(PIPE_M)` 替代 `asc_sync()`，减少不必要的 MTE1/Vector 等待（性能优化，等价安全语义）

### seed 具体修复

- error main.asc:30-34 → fixed main.asc:29-34：在第一次 `asc_mmad(cL0c, ...)` 与第二次 `asc_copy_l12l0a(aL0a, ...)` 之间插入 `asc_sync();`
- error main.asc:23 `__cc__ float* cBad = cL0c + C_BAD_OFFSET;` 删除（fixed 版无 cBad）
- error main.asc:34 `asc_mmad(cBad, ...)` → fixed main.asc:34 `asc_mmad(cL0c, ...)`（恢复正常 L0C 写址）

### 代码模板

模板 1：安全的 Cube TC-Low 流水线（MTE1 写 L0A ↔ PIPE_M 读 L0A 同步）

```cpp
asc_copy_l12l0a(aL0a, aL1, 0, (uint8_t)repeatK, (uint16_t)repeatM, 0);
asc_copy_l12l0b(bL0b, bL1, 0, (uint8_t)repeatN, (uint16_t)repeatK, 0);
asc_mmad(cL0c, aL0a, bL0b, M, N, K, 0, false, false, false);

asc_sync();   // 【关键】全管线屏障，等待 PIPE_M 读 L0A 完成

asc_copy_l12l0a(aL0a, aL1, 0, (uint8_t)repeatK, (uint16_t)repeatM, 0);
asc_mmad(cL0c, aL0a, bL0b, M, N, K, 0, false, false, false);
```

模板 2：封装同步保护（detector 补充）

```cpp
__aicore__ inline void SafeRewriteL0A(
    __ca__ half* aL0a, __cbuf__ half* aL1,
    uint8_t repeatK, uint16_t repeatM)
{
    asc_sync();   // 确保 PIPE_M 已读完 L0A
    asc_copy_l12l0a(aL0a, aL1, 0, repeatK, repeatM, 0);
}
```

### 防御性编程建议

1. **每次重写 L0A 前强制 asc_sync()**：无论前序是 `asc_mmad` 还是另一次 `asc_copy_l12l0a`，重写 L0A 前必须插入 `asc_sync()` 等待 PIPE_M/MTE1 完成
2. **K 维放大时格外警惕**：K > 16 使 PIPE_M 读窗口拉长，与紧接的 MTE1 写 L0A 重叠概率显著上升
3. **L0A/L0B 同等对待**：L0A 与 L0B 共享 Cube 流水线同步约束（参照 l0b_rdwr_conflict），L0A 侧重写同样需要屏障
4. **避免在同一 `__cube__` 函数内混入跨边界写址**：如 `cBad = cL0c + 32640` 等 L0C OOB 注入会触发 L13 兜底，干扰 L53 主路径诊断
5. **代码审查检查清单**：每个 `asc_copy_l12l0a` 调用点回溯前序 `asc_mmad`，确认两者间存在 `asc_sync()`
6. **TC-Low 平坦路径无 EnQue/DeQue 自动同步**：与 AscendC TQue 路径不同，TC-Low `__cube__` 函数需显式 `asc_sync()` 提供同步屏障

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| asc_mmad 后无 asc_sync() 重写 L0A（基准） | cann-aicore/new_case/l0a_rdwr_conflict | `asc_mmad → asc_copy_l12l0a` 同一 aL0a，无屏障 | EC1 |
| 连续 asc_copy_l12l0a 写 L0A 无同步 | 变体（待生成） | 两次 `asc_copy_l12l0a` 间穿插 `asc_mmad` 读，全程无屏障 | EC1 |
| K 维放大 PIPE_M 长读窗口重叠 | 变体（待生成） | K > 16 使 repeatK ≥ 2，PIPE_M 读窗口拉长 | EC2 |
| L0B 侧对称冲突 | cann-aicore/06_cube_conflict/l0b_rw_conflict_verified | L0B buffer 读写冲突（见 l0b_rdwr_conflict） | EC1 |

## 等价类划分

变异维度：Cube 流水线同步屏障的完整性 + PIPE_M 读窗口长度（K 维放大）

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | 单一 L0A buffer 读写冲突（asc_mmad / asc_copy_l12l0a 间无 asc_sync()） | 单一 buffer 连续操作，K 任意 | 编译期可确定屏障缺失，覆盖原始种子 |
| EC2 | K 维放大 PIPE_M 长读窗口与 MTE1 写重叠 | K > 16（repeatK ≥ 2） | 读窗口长度放大冲突概率，运行时显著 |

划分依据：Cube 流水线对 L0A buffer 读写同步的检查。EC1 为编译期可确定的同步屏障缺失，EC2 进一步按 PIPE_M 读窗口长度（K 维）细分。L0B 侧对称冲突归入 l0b_rdwr_conflict，本 EC 表仅覆盖 L0A 侧。

## 关联异常

> 与 T2/T3 同口径，仅取自 references/indices/ 反查（标注反查源）；推断性内容前置标注。

- **cube_l0a_rdwr_cflt / L0A**：共享 keyword（keyword_index 反查：`cube_l0a_rdwr_cflt → [l0a_rdwr_conflict]`（报错描述）、`L0A → [l0a_rdwr_conflict]`（异常名称））。
- **API 反查**：l0a_rdwr_conflict 实测 API 为 `asc_copy_l12l0a` / `asc_mmad` / `asc_sync`（TC-Low 平坦路径），seed_mapping 原标注 "多一次 LoadData 调用" 对应到 `asc_copy_l12l0a`（MTE1 写 L0A 的 LoadData 等价语义）。
- **（推断，非 cross_references 收录）l0b_rdwr_conflict**：对称异常 —— seed_mapping.md:22 声明 l0a_rdwr_conflict "基于 l0b_rdwr_conflict 对称改造，L0A 侧冲突"，l0b_rdwr_conflict 为 L0B 侧冲突（`references/detectors/runtime/cube/l0b_rdwr_conflict.md`）。二者共享 Cube 流水线同步缺失机制；区分：l0a_rdwr_conflict 走 TC-Low 平坦 `__cube__` 路径（asc_sync 屏障），l0b_rdwr_conflict 走 AscendC TQue 路径（EnQue/DeQue 屏障）。
- **（推断，非 cross_references 收录）cube_l0c_overflow / L13 兜底**：error 版 cBad 偏移 32640 跨 L0C 128KB 边界，作为 L13 路径兜底保证 EZ9999，与 L53 主路径并存（见 `references/detectors/runtime/cube/cube_l0c_overflow.md`）。

## 检测执行指引

- **适用匹配模式**：API 调用模式（RULE-01/02）、值域判断（RULE-03）
- **命中判定**：
  - API 调用模式类规则——构建 `__cube__` 函数内调用序列（asc_copy_l12l0a / asc_mmad / asc_sync / pipe_barrier），按 L0A buffer 实例（aL0a）聚合；`asc_mmad(*, aL0a, ...)` 与后续 `asc_copy_l12l0a(aL0a, ...)` 之间缺失 `asc_sync()` / `pipe_barrier` 即命中 RULE-01；连续两次 `asc_copy_l12l0a(aL0a, ...)` 间无屏障即命中 RULE-02；CFG 跨函数追溯受限时标记 `trace_limited=true`
  - 值域判断类规则（RULE-03）——提取 `asc_mmad` 的 K 参数（或 repeatK = (K+15)/16），K > 16 且存在上述无屏障调用序列即放大命中权重
- **修复映射**：
  - l0a_rdwr_conflict-RULE-01（asc_mmad 后无 asc_sync() 重写 L0A）→ 修复模式 1（插入 asc_sync()）
  - l0a_rdwr_conflict-RULE-02（连续 asc_copy_l12l0a 无同步）→ 修复模式 1（每次重写 L0A 前插入 asc_sync()）/ 修复模式 3（定向 PIPE_M 屏障）
  - l0a_rdwr_conflict-RULE-03（K 维放大重叠）→ 修复模式 1（K > 16 时强制 asc_sync()）
