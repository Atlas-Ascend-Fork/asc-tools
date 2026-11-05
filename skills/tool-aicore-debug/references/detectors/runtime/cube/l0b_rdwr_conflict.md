# l0b_rdwr_conflict — L0B 读写冲突

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | l0b_rdwr_conflict |
| 异常类型 | cube_l0b_rdwr_cflt（双错误码之一为 mte_l0b_rdwr_cflt） |
| 错误码 | 14 / 39（双错误码：14 = cube_l0b_rdwr_cflt，39 = mte_l0b_rdwr_cflt） |
| 类别 | cube |
| 源文件 | cann-aicore/06_cube_conflict/l0b_rw_conflict_verified/error/main.asc |
| 报错模式 | EZ9999_stderr |
| 内存区域 | L0_B（L0B buffer 读写冲突） |
| kernel_diff_type | kernel_diff |
| behavior_note | 双错误码: 14/39 |

## 触发条件

Cube 单元的 L0B buffer 在同一时刻被 LoadData 写入和 Mmad 读取，导致读写冲突。根本原因在于连续两次 LoadData 写入 L0B 时缺少 EnQue/DeQue 同步屏障，Cube 流水线中 qA1→qA2→qB1→qB2→qCO1 的配对关系被破坏，数据一致性无法保证。

触发条件（detector 原文，逐字保留）：

- 连续两次 LoadData 写入同一 L0B buffer 无同步屏障
- LoadData 写 L0B 后直接执行 Mmad 读取 L0B 无同步
- L0A buffer 存在类似的读写冲突模式
- Cube 流水线中多级队列同步缺失

形式化规则前提：`ASSERT l0b_rdwr_conflict-SYNC-01: ∀ LoadData 写入 L0B_BUF 后的后续操作 O 直接使用 L0B_BUF, ∃ EnQue(L0B_BUF) ∧ DeQue(&L0B_BUF) 在两者之间`；`ASSERT l0b_rdwr_conflict-SYNC-02: ∀ 连续 LoadData 操作同一 buffer, ∃ EnQue/DeQue 同步屏障`。

## 检测规则

### l0b_rdwr_conflict-RULE-01：连续 LoadData 无同步

**【条件】**对同一 buffer（如 a2）的连续 LoadData 调用，两次 LoadData 之间缺少 EnQue/DeQue 同步操作；且 LoadData 与 Mmad 之间缺少同步屏障
**【API】**LoadData（连续调用同一 buffer）/ EnQue / DeQue
**【约束】**连续两次 LoadData 之间存在 EnQue/DeQue 同步屏障
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

匹配示例（detector 原文）：

```cpp
LoadData(a2, a1d, ldParams);
LoadData(a2, a1d, ldParams);
Mmad(c1, a2, b2d, mmadParams);
```

### l0b_rdwr_conflict-RULE-02：LoadData 后直接 Mmad 无同步

**【条件】**LoadData 写入 buffer 后紧接 Mmad 读取同一 buffer，中间缺少 EnQue/DeQue 同步操作
**【API】**LoadData / Mmad（L0A/L0B buffer 写入与 Cube 计算间同步）
**【约束】**LoadData 写入 buffer 后存在 EnQue/DeQue 同步屏障再执行 Mmad 读取
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

### l0b_rdwr_conflict-RULE-03：Cube 流水线队列配对不完整

**【条件】**Cube 流水线 qA1→qA2→qB1→qB2→qCO1 的 EnQue/DeQue 配对不完整，某 EnQue 后无对应 DeQue，或 buffer 操作前未通过 DeQue 获取
**【API】**TQue::EnQue / TQue::DeQue（Cube 流水线多级队列）
**【约束】**`∀ Cube 流水线: qA1→qA2→qB1→qB2→qCO1 配对完整 ∧ 每个 EnQue 有对应 DeQue ∧ buffer 使用前已通过 DeQue 获取`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

匹配示例（detector 原文）：

```cpp
qA2.EnQue(a2);
LoadData(a2, a1d, ldParams);
```

### l0b_rdwr_conflict-RULE-04：流水线级间同步缺失（运行时检测）

**【条件】**Cube 流水线各级之间同步点不充分，qA1、qA2、qB1、qB2、qCO1 队列操作序列不正确，存在跨级操作缺少中间同步
**【API】**Cube 流水线各级 Queue 操作
**【约束】**各级 Queue 操作序列正确，跨级操作存在中间同步
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

### l0b_rdwr_conflict-RULE-05：循环中的同步缺失

**【条件】**循环体内 LoadData 和 Mmad 缺少同步保护，每次迭代中的队列操作配对不完整
**【API】**LoadData / Mmad（循环内调用）
**【约束】**循环每次迭代中 LoadData 和 Mmad 之间存在同步保护，队列操作配对完整
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

匹配示例（detector 原文）：

```cpp
for (int i = 0; i < tileNum; i++) {
    LoadData(a2, a1d, ldParams);
    Mmad(c1, a2, b2d, mmadParams);
}
```

## 修复方案

### 标准修复模式

1. **修复模式 1（连续 LoadData 之间插入 EnQue/DeQue 同步）**：在连续两次 LoadData 之间插入 `qA2.EnQue(a2); qA2.DeQue(&a2);`
2. **修复模式 2（LoadData 与 Mmad 之间添加同步）**：在 LoadData 写入后、Mmad 读取前插入 `qA2.EnQue(a2); qA2.DeQue(&a2);`
3. **修复模式 3（完整的 Cube 流水线同步）**：每一级 LoadData 后插入对应 Queue 的 EnQue/DeQue
4. **修复模式 4（循环体内的同步保护）**：循环每次迭代的 LoadData 和 Mmad 之间插入同步，且每次迭代末尾 EnQue/DeQue 输出 c1

### 代码模板

模板：标准 Cube 流水线操作序列（detector 原文）

```cpp
__aicore__ inline void CubePipelineLoadCompute(
    TBuf<QuePosition::A2>& a2,
    TBuf<QuePosition::B2>& b2,
    TBuf<QuePosition::CO1>& c1,
    LocalTensor<half>& a1d,
    LocalTensor<half>& b1d,
    LoadData2DParams& ldParams,
    MmadParams& mmadParams)
{
    LoadData(a2, a1d, ldParams);
    qA2.EnQue(a2);
    qA2.DeQue(&a2);

    LoadData(b2, b1d, ldParams);
    qB2.EnQue(b2);
    qB2.DeQue(&b2);

    Mmad(c1, a2, b2, mmadParams);
    qCO1.EnQue(c1);
    qCO1.DeQue(&c1);
}
```

模板：安全的 LoadData 封装（detector 原文）

```cpp
template<typename T>
__aicore__ inline void SafeLoadDataSync(
    TBuf<QuePosition::A2>& buf,
    LocalTensor<T>& src,
    LoadData2DParams& params,
    TQue<QuePosition::A2, 1>& queue)
{
    LoadData(buf, src, params);
    queue.EnQue(buf);
    queue.DeQue(&buf);
}
```

### 防御性编程建议

1. **每次 LoadData 后必须同步**：无论后续是再次 LoadData 还是 Mmad，都需插入 EnQue/DeQue
2. **遵循 Cube 流水线队列配对规则**：qA1→qA2→qB1→qB2→qCO1，每级操作前后确保同步
3. **封装 LoadData+同步操作**：将 LoadData + EnQue + DeQue 封装为工具函数，避免遗漏
4. **循环体内严格同步**：每次迭代的 LoadData 和 Mmad 之间必须有同步屏障
5. **L0A 和 L0B 同等对待**：L0A buffer 同样遵循相同的同步约束，不可忽略
6. **代码审查检查清单**：每个 LoadData 和 Mmad 的调用点都需检查同步操作是否存在

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| 连续两次 LoadData 无同步 | cann-aicore/06_cube_conflict/l0b_rw_conflict_verified | LoadData → LoadData → Mmad，中间无任何同步 | EC1 |
| LoadData 后直接 Mmad 无同步 | 变体（待生成） | LoadData → Mmad，缺少 EnQue/DeQue 屏障 | EC1 |
| L0A 类似冲突 | 变体（待生成） | 对 L0A buffer 的连续 LoadData 和 Mmad 缺少同步 | EC1 |
| 多级流水线同步缺失 | 变体（待生成） | qA1→qA2→qB1→qB2→qCO1 多级队列同步不完整 | EC2 |

## 等价类划分

变异维度：Cube 流水线同步屏障的完整性 + 流水线级数

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | 单一 buffer 读写冲突（LoadData/LoadData 或 LoadData/Mmad 间无同步） | 单一 buffer 连续操作 | 编译期可确定，覆盖原始种子 |
| EC2 | 多级流水线同步缺失 | 跨级 Queue 操作配对不完整 | 需流水线分析 |

划分依据：Cube 流水线对 L0B（及 L0A）buffer 读写同步的检查。EC1 为单一 buffer 编译期可确定的同步缺失，EC2 需多级流水线数据流分析。

## 关联异常

- **cube_l0b_rdwr_cflt / mte_l0b_rdwr_cflt / L0B**：共享 keyword（cross_references 反查 keyword_index：cube_l0b_rdwr_cflt → [l0b_rdwr_conflict]，mte_l0b_rdwr_cflt → [l0b_rdwr_conflict]，L0B → [l0b_rdwr_conflict]）。l0b_rdwr_conflict 为双错误码异常（14 = cube 侧，39 = mte 侧）。
- **l0a_rdwr_conflict**：对称异常（推断，非 cross_references 收录）—— l0a_rdwr_conflict 为 L0A 侧冲突（基于 l0b_rdwr_conflict 对称改造），l0b_rdwr_conflict 为 L0B 侧冲突；共享 Cube 流水线同步缺失机制。
- **（推断，非 cross_references 收录）**与 alloc_free_mismatch/enque_deque_mismatch 共享"同步屏障缺失导致硬件冲突/阻塞"语义模式（l0b_rdwr_conflict = LoadData/EnQue/DeQue 同步缺失导致 L0B 读写冲突，alloc_free_mismatch = AllocTensor/FreeTensor 不配对导致阻塞，enque_deque_mismatch = EnQue/DeQue 不配对导致阻塞）。

## 检测执行指引

- **适用匹配模式**：API 调用模式（RULE-01/02/03/04/05）
- **命中判定**：
  - API 调用模式类规则——构建 Cube 流水线调用序列（LoadData / Mmad / EnQue / DeQue），按 buffer 实例（a2/b2/c1）聚合；连续两次 LoadData 同一 buffer 之间或 LoadData 与 Mmad 之间缺少 EnQue/DeQue 同步即命中；CFG 跨函数追溯受限时标记 `trace_limited=true`；多级流水线验证 `qA1→qA2→qB1→qB2→qCO1` 每个 EnQue 有对应 DeQue
  - 循环场景（RULE-05）统计单次迭代内的 LoadData/Mmad/EnQue/DeQue 调用序列，验证同步屏障存在
- **修复映射**：
  - l0b_rdwr_conflict-RULE-01（连续 LoadData 无同步）→ 修复模式 1（连续 LoadData 间插入 EnQue/DeQue）
  - l0b_rdwr_conflict-RULE-02（LoadData 后直接 Mmad）→ 修复模式 2（LoadData 与 Mmad 间添加同步）
  - l0b_rdwr_conflict-RULE-03（流水线队列配对不完整）→ 修复模式 3（完整 Cube 流水线同步）
  - l0b_rdwr_conflict-RULE-04（流水线级间同步缺失）→ 修复模式 3
  - l0b_rdwr_conflict-RULE-05（循环内同步缺失）→ 修复模式 4（循环体内同步保护）
