# enque_deque_mismatch — EnQue/DeQue 不配对

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | enque_deque_mismatch |
| 异常类型 | kernel hangs（永久阻塞） |
| 错误码 | 无（kernel 挂起型） |
| 类别 | sync_hang |
| 源文件 | cann-aicore/03_sync/enque_deque_mismatch_hangs/error/main.asc |
| 报错模式 | kernel_hangs |
| 内存区域 | 多区域（Queue 流水线跨级） |
| kernel_diff_type | kernel_diff |
| behavior_note | kernel挂起型，无EZ9999输出 |

## 触发条件

在昇腾 NPU 编程模型中，Queue（如 `qA1`、`qB1`、`qCO1` 等）是连接不同计算阶段的数据通道。`EnQue` 将 buffer 放入队列尾部，`DeQue` 从队列头部取出 buffer。当队列为空时调用 `DeQue`，该操作将阻塞等待，直到有新的 buffer 被 `EnQue` 放入。当 `EnQue` 调用次数少于 `DeQue` 调用次数，或在 `DeQue` 执行前未执行 `EnQue` 时，`DeQue` 将在空队列上永久阻塞。在多级队列流水线场景中（如 qA1→qA2→qB1→qB2→qCO1），任何一级队列的配对断裂都会导致后续所有阶段的 DeQue 连锁阻塞（cross_references "症状相似"段：kernel hangs = {flag_mismatch, alloc_free_mismatch, enque_deque_mismatch, padding_misconfig}，区分要点：enque_deque_mismatch = EnQue 后无 DeQue）。

触发条件（detector 原文，逐字保留）：

- DeQue 在 EnQue 之前执行，导致队列为空时的阻塞等待
- 循环中 `EnQue` 调用次数少于 `DeQue` 调用次数，经过若干迭代后队列被耗尽
- 多级队列流水线中某一级的 EnQue 缺失，导致下游所有队列的 DeQue 连锁阻塞
- 条件分支中 `EnQue` 被跳过，但 `DeQue` 在分支汇合后仍被执行

形式化规则 enque_deque_mismatch-F01 前提：`∀ Queue Q: COUNT(Q.EnQue) == COUNT(Q.DeQue)`；enque_deque_mismatch-F02 时序前提：`∀ DeQue 点 D 的所有前驱路径 P 上存在 EnQue 且在 D 之前`。

## 检测规则

### enque_deque_mismatch-RULE-01：孤立 DeQue 检测

**【条件】**函数或代码块中 `DeQue` 被调用但在该调用之前不存在对应的 `EnQue`，或整个编译单元中 EnQue 调用次数为 0
**【API】**TQue::EnQue / TQue::DeQue
**【约束】**对每个 Queue 对象，DeQue 调用点的前驱路径上存在 EnQue；不存在函数入口点直接调用 DeQue 的场景
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

匹配示例（detector 原文）：

```cpp
LocalTensor<T> buf = qA1.DeQue<T>();  // 无前置 EnQue
```

### enque_deque_mismatch-RULE-02：循环内 EnQue/DeQue 次数匹配检测

**【条件】**循环体内 `EnQue` 和 `DeQue` 的调用次数不一致（每次迭代中 EnQue 调用次数 ≠ DeQue 调用次数）
**【API】**TQue::EnQue / TQue::DeQue（循环内计数）
**【约束】**`∀ 循环 L: per_iter_enque == per_iter_deque`，且 EnQue 次数 ≥ DeQue 次数
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

匹配示例（detector 原文）：

```cpp
for (int i = 0; i < N; i++) {
    if (cond) {
        qA1.EnQue(buf);     // 条件性 EnQue
    }
    LocalTensor<T> b = qA1.DeQue<T>();  // 无条件 DeQue
}
```

### enque_deque_mismatch-RULE-03：多级队列流水线完整性检测

**【条件】**多级队列流水线（qA1→qA2→qB1→qB2→qCO1）中，某一级队列的 EnQue 缺失或 DeQue 多余，导致流水线断裂
**【API】**TQue::EnQue / TQue::DeQue（流水线数据流图）
**【约束】**`∀ 流水线边 (Q_src → Q_dst): COUNT(Q_src.EnQue) >= COUNT(Q_dst.DeQue)`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

匹配示例（detector 原文）：

```cpp
// 第一级：qA1 出队，处理后入队 qA2
LocalTensor<T> buf1 = qA1.DeQue<T>();
Process(buf1);
// 缺少 qA2.EnQue(buf1);

// 第二级：从 qA2 出队（将阻塞）
LocalTensor<T> buf2 = qA2.DeQue<T>();
```

### enque_deque_mismatch-RULE-04：时序约束（EnQue 先于 DeQue）

**【条件】**存在 DeQue 调用点 D，从程序入口到 D 的某条控制流路径 P 上不存在 EnQue 或 EnQue 在 D 之后
**【API】**TQue::EnQue / TQue::DeQue（CFG 时序分析）
**【约束】**`∀ DeQue 点 D 的所有前驱路径 P 上 ∃ EnQue 且在 D 之前`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

### enque_deque_mismatch-RULE-05：动态队列状态监控（运行时检测）

**【条件】**运行时插桩监控每个 Queue 对象的当前 buffer 数量，DeQue 被调用时检查队列是否为空
**【API】**TQue::EnQue / TQue::DeQue（运行时计数器以 Queue 对象为 key）
**【约束】**DeQue 调用时 `queue_size(Q) > 0`；循环结束时 `queue_size(Q) == 0`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

## 修复方案

### 标准修复模式

1. **修复模式 enque_deque_mismatch-FIX-01（补全缺失的 EnQue）**：DeQue 存在但缺少对应 EnQue 时，在 DeQue 前补全 EnQue
2. **修复模式 enque_deque_mismatch-FIX-02（循环中 EnQue/DeQue 次数匹配修正）**：循环中 EnQue 次数少于 DeQue 次数时，将 EnQue 提为无条件调用
3. **修复模式 enque_deque_mismatch-FIX-03（多级队列流水线断裂修复）**：多级队列传递中某一级缺少 EnQue 时，在 ProcessStage 后补全下一级 EnQue
4. **修复模式 enque_deque_mismatch-FIX-04（条件分支路径补全 EnQue）**：条件分支中部分路径缺少 EnQue 但 DeQue 无条件执行时，将 DeQue 也放入条件分支内部，或在 else 分支补全 FreeTensor

### 代码模板

模板 enque_deque_mismatch-TPL-01：标准单级队列流水线模式（detector 原文）

```cpp
template<typename T, QuePosition POS, int BUFFER_COUNT>
void pipeline_stage(TQue<POS, BUFFER_COUNT>& queue, int count) {
    for (int i = 0; i < count; i++) {
        LocalTensor<T> buf = queue.AllocTensor<T>();
        FillData(buf, i);
        queue.EnQue(buf);
        LocalTensor<T> out = queue.DeQue<T>();
        ProcessData(out);
        queue.FreeTensor(out);
    }
}
```

模板 enque_deque_mismatch-TPL-02：多级队列流水线模板（detector 原文）

```cpp
template<typename T, QuePosition POS1, QuePosition POS2, int BC>
void two_stage_pipeline(TQue<POS1, BC>& qIn, TQue<POS2, BC>& qOut, int count) {
    for (int i = 0; i < count; i++) {
        LocalTensor<T> buf1 = qIn.AllocTensor<T>();
        FillData(buf1, i);
        qIn.EnQue(buf1);
        LocalTensor<T> buf2 = qIn.DeQue<T>();
        ProcessStage1(buf2);
        qOut.EnQue(buf2);
        LocalTensor<T> buf3 = qOut.DeQue<T>();
        ProcessStage2(buf3);
        qOut.FreeTensor(buf3);
        qIn.FreeTensor(buf2);
    }
}
```

### 防御性编程建议

1. **严格遵循流水线模式**：始终按照 `AllocTensor → 填充数据 → EnQue → DeQue → 处理数据 → FreeTensor` 的顺序操作 Queue，不跳过任何步骤。
2. **流水线级间一致性保证**：在多级队列流水线中，确保每一级的输出（EnQue）数量严格等于下一级的输入（DeQue）数量，构建流水线时用表格或图示明确每级的入队/出队关系。
3. **条件分支中避免部分入队**：若 DeQue 在分支汇合后执行，则 EnQue 必须在所有分支路径上执行。若无法保证所有路径入队，则将 DeQue 也放入条件分支内部。
4. **静态分析集成**：在 CI/CD 流水线中集成静态分析工具，自动检测每个 Queue 对象的 EnQue/DeQue 配对完整性，包括循环内和条件分支路径。
5. **流水线设计文档**：在设计多级队列流水线时，绘制完整的数据流图，标注每级队列的 EnQue 和 DeQue 调用点，并在代码审查时对照设计图检查。
6. **运行时队列状态断言**：在调试模式下，在 DeQue 前添加断言检查队列非空（如 `ASSERT(queue.Size() > 0)`），帮助在开发阶段及早发现配对问题。

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| 直接 DeQue 空队列 | cann-aicore/03_sync/enque_deque_mismatch_hangs | 函数入口直接 DeQue，无前置 EnQue | EC1 |
| EnQue 次数少于 DeQue 次数 | 变体（待生成） | 循环中 EnQue 总次数 < DeQue 总次数 | EC2 |
| 多级队列配对断裂 | 变体（待生成） | 流水线某一级 EnQue 缺失，下游连锁阻塞 | EC3 |
| 条件分支导致 EnQue 被跳过 | 变体（待生成） | if/else 中 EnQue 仅部分路径，DeQue 无条件执行 | EC4 |

## 等价类划分

变异维度：EnQue/DeQue 配对完整性 + 时序 + 流水线级间一致性

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | 直接 DeQue 空队列 | enque_count = 0 | 最简单配对缺失，覆盖原始种子 |
| EC2 | 循环内次数不匹配 | per_iter_enque < per_iter_deque | 需循环体内计数分析 |
| EC3 | 多级流水线断裂 | 流水线某级 src_enque < dst_deque | 需流水线数据流图 |
| EC4 | 条件分支时序错误 | 部分前驱路径无 EnQue 但 DeQue 无条件 | 需 CFG 时序分析 |

划分依据：detector detection_rules "变体检测覆盖"表 4 类变体一一对应（可检测性由高到低：EC1/EC2 静态可完全检测，EC3/EC4 需流水线数据流图或完整 CFG 分析）。

## 关联异常

- **flag_mismatch / alloc_free_mismatch / padding_misconfig**：同属 cross_references "症状相似"段：kernel hangs = {flag_mismatch, alloc_free_mismatch, enque_deque_mismatch, padding_misconfig}。区分要点（cross_references 原文）：flag_mismatch = flag 不配对；alloc_free_mismatch = AllocTensor 后无 FreeTensor；enque_deque_mismatch = EnQue 后无 DeQue；padding_misconfig = 有错误码 41 但实际挂起。
- **alloc_free_mismatch**：同属 Queue 内存管理类，alloc_free_mismatch 关注 AllocTensor/FreeTensor 配对，enque_deque_mismatch 关注 EnQue/DeQue 配对（语义相关，推断）。
- **PipeBarrier / EnQue / DeQue**：共享同步与内存管理类 API（cross_references 反查 api_index：PipeBarrier → [flag_mismatch, alloc_free_mismatch, enque_deque_mismatch]，EnQue → [enque_deque_mismatch]，DeQue → [enque_deque_mismatch]）；**Queue**：共享内存管理类关键词（cross_references 反查 keyword_index：Queue → [alloc_free_mismatch, enque_deque_mismatch]）。
- **（推断，非 cross_references 收录）**与 alloc_free_mismatch 共享 Queue 容量耗尽导致阻塞的机制，但 alloc_free_mismatch 是 AllocTensor 阻塞、enque_deque_mismatch 是 DeQue 阻塞。

## 检测执行指引

- **适用匹配模式**：存在性检查（RULE-01）、API 调用模式（RULE-02/03/04/05）
- **命中判定**：
  - 存在性检查类规则——全文模式搜索 `Q.DeQue` 调用，反查同一 Queue 对象是否存在前置 `Q.EnQue`；函数入口点直接 DeQue 即命中；跨行追溯 ≤3 层
  - API 调用模式类规则——按 Queue 对象实例聚合 EnQue/DeQue 调用计数，循环体内统计单次迭代调用次数；流水线边 (Q_src→Q_dst) 验证 `src_enque >= dst_deque`；CFG 时序分析时跨函数追溯受限，标记 `trace_limited=true`
- **修复映射**：
  - enque_deque_mismatch-RULE-01（孤立 DeQue）→ 修复模式 enque_deque_mismatch-FIX-01（补全 EnQue）
  - enque_deque_mismatch-RULE-02（循环内次数匹配）→ 修复模式 enque_deque_mismatch-FIX-02（次数匹配修正）
  - enque_deque_mismatch-RULE-03（多级流水线完整性）→ 修复模式 enque_deque_mismatch-FIX-03（流水线断裂修复）
  - enque_deque_mismatch-RULE-04（时序约束）→ 修复模式 enque_deque_mismatch-FIX-01 / enque_deque_mismatch-FIX-04
  - enque_deque_mismatch-RULE-05（动态队列监控）→ 用于运行时定位，结合 enque_deque_mismatch-FIX-01/02/03/04 修复
