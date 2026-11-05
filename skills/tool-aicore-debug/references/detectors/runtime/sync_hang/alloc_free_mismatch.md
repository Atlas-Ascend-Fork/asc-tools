# alloc_free_mismatch — AllocTensor/FreeTensor 不配对

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | alloc_free_mismatch |
| 异常类型 | kernel hangs（永久阻塞） |
| 错误码 | 无（kernel 挂起型） |
| 类别 | sync_hang |
| 源文件 | cann-aicore/03_sync/alloc_free_mismatch_hangs/error/main.asc |
| 报错模式 | kernel_hangs |
| 内存区域 | 多区域（Queue buffer 跨流水线级） |
| kernel_diff_type | kernel_diff |
| behavior_note | kernel挂起型，无EZ9999输出 |

## 触发条件

在昇腾 NPU 编程模型中，每个 Queue（如 `qA1`、`qB1` 等）拥有固定数量的 buffer 资源（通常为 2 个或 4 个）。`AllocTensor` 从 Queue 中分配一个 buffer，`FreeTensor` 将 buffer 归还 Queue。两者必须严格配对使用。当 `AllocTensor` 调用次数多于 `FreeTensor` 调用次数时，Queue 中的 buffer 将逐渐被耗尽，一旦全部被占用，后续的 `AllocTensor` 调用将永久阻塞（cross_references "症状相似"段：kernel hangs = {flag_mismatch, alloc_free_mismatch, enque_deque_mismatch, padding_misconfig}，区分要点：alloc_free_mismatch = AllocTensor 后无 FreeTensor）。

触发条件（detector 原文，逐字保留）：

- 循环中 `AllocTensor` 被调用但无配对的 `FreeTensor`，导致 buffer 逐渐耗尽
- 条件分支中某些路径执行了 `AllocTensor` 但在退出前未调用 `FreeTensor`
- 错误处理路径中跳过了 `FreeTensor`，导致已分配的 buffer 无法回收
- 嵌套循环中内层 `AllocTensor` 的 buffer 未在内层释放，累积到外层后超出 Queue 容量

形式化规则 alloc_free_mismatch-F01 前提：`∀ Queue Q: COUNT(Q.AllocTensor) == COUNT(Q.FreeTensor)`。

## 检测规则

### alloc_free_mismatch-RULE-01：作用域内 AllocTensor/FreeTensor 配对检测

**【条件】**同一函数或代码块作用域中 `AllocTensor` 被调用但不存在对应的 `FreeTensor` 调用，或两者调用次数不一致
**【API】**TQue::AllocTensor / TQue::FreeTensor
**【约束】**对每个 Queue 对象，非循环代码块中 AllocTensor 与 FreeTensor 调用次数相等；循环代码块中每次迭代的调用次数相等
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

匹配示例（detector 原文）：

```cpp
LocalTensor<T> buf = qA1.AllocTensor<T>();
// 无 qA1.FreeTensor(buf);
```

### alloc_free_mismatch-RULE-02：条件分支路径 FreeTensor 覆盖检测

**【条件】**`AllocTensor` 在分支前调用，但 `FreeTensor` 仅出现在 if/else 的部分分支中，某些路径上 buffer 未被释放
**【API】**TQue::AllocTensor / TQue::FreeTensor（CFG 路径分析）
**【约束】**`AllocTensor` 调用点后所有到达函数退出点的控制流路径上都存在对同一 buffer 变量的 `FreeTensor` 调用
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

匹配示例（detector 原文）：

```cpp
LocalTensor<T> buf = qA1.AllocTensor<T>();
if (cond) {
    qA1.FreeTensor(buf);   // 仅 cond==true 时释放
}
// cond==false 时 buf 泄漏
```

### alloc_free_mismatch-RULE-03：跳转语句资源泄漏检测

**【条件】**`AllocTensor` 分配 buffer 后，在 `FreeTensor` 之前存在 `continue`、`break`、`return` 或 `goto` 等跳转语句，导致 FreeTensor 被跳过
**【API】**TQue::AllocTensor / TQue::FreeTensor（跳转语句检测）
**【约束】**`AllocTensor` 与 `FreeTensor` 之间的代码段不存在可能跳过 FreeTensor 的控制流转移
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** 存在性检查

匹配示例（detector 原文）：

```cpp
for (int i = 0; i < N; i++) {
    LocalTensor<T> buf = qA1.AllocTensor<T>();
    if (error_condition) {
        continue;  // 跳过后续的 FreeTensor
    }
    qA1.FreeTensor(buf);
}
```

### alloc_free_mismatch-RULE-04：循环迭代内配对性

**【条件】**循环结构包含 AllocTensor 或 FreeTensor，单次迭代中两者调用次数不相等
**【API】**TQue::AllocTensor / TQue::FreeTensor（循环内计数）
**【约束】**`∀ 循环 L: per_iter_alloc == per_iter_free`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

### alloc_free_mismatch-RULE-05：动态 buffer 使用量监控（运行时检测）

**【条件】**运行时插桩统计每个 Queue 对象的 buffer 使用量（已分配但未释放），使用量超过 Queue 容量或循环结束时使用量不为零
**【API】**TQue::AllocTensor / TQue::FreeTensor（运行时计数器以 Queue 对象为 key）
**【约束】**`∀ Queue Q: 0 ≤ in_use_count(Q) ≤ capacity(Q)`，循环结束时 `in_use_count(Q) == 0`
**【来源】**LLM 推断 <!-- TODO: validate 时人工抽检 -->
**【匹配模式】** API 调用模式

## 修复方案

### 标准修复模式

1. **修复模式 alloc_free_mismatch-FIX-01（补全循环中的 FreeTensor）**：循环体内 AllocTensor 后缺少 FreeTensor 时，在循环末尾补全
2. **修复模式 alloc_free_mismatch-FIX-02（条件分支路径补全 FreeTensor）**：条件分支部分路径缺少 FreeTensor 时，将 FreeTensor 提到分支汇合点之后统一调用
3. **修复模式 alloc_free_mismatch-FIX-03（错误处理路径补全）**：错误处理路径中遗漏 FreeTensor 时，在 return 前补全
4. **修复模式 alloc_free_mismatch-FIX-04（嵌套循环 buffer 管理修正）**：嵌套循环中内外层 AllocTensor/FreeTensor 不匹配时，在内层循环末尾补全内层 FreeTensor

### 代码模板

模板 alloc_free_mismatch-TPL-01：标准 AllocTensor/FreeTensor 配对模式（detector 原文）

```cpp
template<typename T, QuePosition POS, int BUFFER_COUNT>
void process_with_buffer(TQue<POS, BUFFER_COUNT>& queue) {
    LocalTensor<T> buf = queue.AllocTensor<T>();
    // 数据处理
    queue.FreeTensor(buf);
}
```

模板 alloc_free_mismatch-TPL-02：RAII 风格 buffer 管理模板（detector 原文）

```cpp
template<typename T, QuePosition POS, int BUFFER_COUNT>
class ScopedBuffer {
public:
    ScopedBuffer(TQue<POS, BUFFER_COUNT>& queue) : queue_(queue) {
        buf_ = queue_.AllocTensor<T>();
    }
    ~ScopedBuffer() {
        queue_.FreeTensor(buf_);
    }
    LocalTensor<T> Get() { return buf_; }
    ScopedBuffer(const ScopedBuffer&) = delete;
    ScopedBuffer& operator=(const ScopedBuffer&) = delete;
private:
    TQue<POS, BUFFER_COUNT>& queue_;
    LocalTensor<T> buf_;
};
```

### 防御性编程建议

1. **配对规范强制执行**：编码规范要求 `AllocTensor` 和 `FreeTensor` 必须在同一代码块中成对出现，建议 FreeTensor 紧跟 AllocTensor 所在代码块的末尾。
2. **RAII 封装**：使用 RAII 模式封装 buffer 管理，在构造函数中调用 AllocTensor，在析构函数中调用 FreeTensor，利用作用域规则自动保证配对。
3. **循环内统一释放点**：在循环体中将 FreeTensor 放在循环末尾的统一位置，避免在中间的 continue/break 路径上遗漏释放。
4. **静态分析工具**：集成静态分析工具到 CI/CD 流水线，自动检测每个 Queue 对象上的 AllocTensor/FreeTensor 配对完整性。
5. **代码审查检查项**：在代码审查中重点关注新增的 AllocTensor 调用，确保每条执行路径上都有对应的 FreeTensor。
6. **Queue 容量与循环次数匹配**：在设计阶段评估循环最大迭代次数与 Queue buffer 容量的关系，确保即使存在轻微泄漏也不会在单次 kernel 执行中耗尽 buffer。

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| 循环中 AllocTensor 无配对 FreeTensor | cann-aicore/03_sync/alloc_free_mismatch_hangs | 循环体内 AllocTensor 后缺少 FreeTensor，每次迭代泄漏一个 buffer | EC1 |
| 条件分支部分路径缺少 FreeTensor | 变体（待生成） | if/else 中部分路径未调用 FreeTensor | EC2 |
| 异常路径忘记 FreeTensor | 变体（待生成） | 错误处理分支 return/continue 前未释放 | EC3 |
| 嵌套循环内外层不匹配 | 变体（待生成） | 内层 AllocTensor 未在内层释放，累积到外层 | EC4 |

## 等价类划分

变异维度：AllocTensor/FreeTensor 配对完整性 + 控制流路径覆盖

| 等价类 | 描述 | 边界值范围 | 划分依据 |
|--------|------|-----------|---------|
| EC1 | 循环体内完全缺失 FreeTensor | 每次迭代泄漏 1 个 buffer | 最简单配对缺失，覆盖原始种子 |
| EC2 | 条件分支部分路径缺失 | 部分前驱路径无 FreeTensor | 需 CFG 分析 |
| EC3 | 跳转语句跳过 FreeTensor | return/continue/break 前未释放 | 需识别跳转语句 |
| EC4 | 嵌套循环内外层不匹配 | 内层 buffer 未在内层释放 | 需跨循环分析 |

划分依据：detector detection_rules "变体检测覆盖"表 4 类变体一一对应（可检测性由高到低：EC1 静态可完全检测，EC2/EC3/EC4 需 CFG/跳转/跨循环分析）。

## 关联异常

- **flag_mismatch / enque_deque_mismatch / padding_misconfig**：同属 cross_references "症状相似"段：kernel hangs = {flag_mismatch, alloc_free_mismatch, enque_deque_mismatch, padding_misconfig}。区分要点（cross_references 原文）：flag_mismatch = flag 不配对；alloc_free_mismatch = AllocTensor 后无 FreeTensor；enque_deque_mismatch = EnQue 后无 DeQue；padding_misconfig = 有错误码 41 但实际挂起。
- **enque_deque_mismatch**：同属 Queue 内存管理类，alloc_free_mismatch 关注 AllocTensor/FreeTensor 配对，enque_deque_mismatch 关注 EnQue/DeQue 配对（语义相关，推断）。
- **PipeBarrier / AllocTensor / FreeTensor**：共享同步与内存管理类 API（cross_references 反查 api_index：PipeBarrier → [flag_mismatch, alloc_free_mismatch, enque_deque_mismatch]，AllocTensor/FreeTensor → [alloc_free_mismatch]）；**Queue**：共享内存管理类关键词（cross_references 反查 keyword_index：Queue → [alloc_free_mismatch, enque_deque_mismatch]）。
- **（推断，非 cross_references 收录）**与 enque_deque_mismatch 共享 Queue 容量耗尽导致阻塞的机制，但触发 API 不同。

## 检测执行指引

- **适用匹配模式**：API 调用模式（RULE-01/02/04/05）、存在性检查（RULE-03）
- **命中判定**：
  - API 调用模式类规则——按 Queue 对象实例聚合 AllocTensor/FreeTensor 调用计数，循环体内统计单次迭代调用次数；非循环作用域 alloc_count ≠ free_count 即命中；循环作用域 per_iter_alloc ≠ per_iter_free 即命中；CFG 前驱路径分析时跨函数追溯受限，标记 `trace_limited=true`
  - 存在性检查类规则——在 `AllocTensor(buf)` 与 `FreeTensor(buf)` 之间检测 continue/break/return/goto 跳转语句，存在即命中；跨行追溯 ≤3 层
- **修复映射**：
  - alloc_free_mismatch-RULE-01（作用域内配对）→ 修复模式 alloc_free_mismatch-FIX-01（补全循环 FreeTensor）/ alloc_free_mismatch-FIX-04（嵌套循环修正）
  - alloc_free_mismatch-RULE-02（条件分支覆盖）→ 修复模式 alloc_free_mismatch-FIX-02（条件分支补全）
  - alloc_free_mismatch-RULE-03（跳转语句泄漏）→ 修复模式 alloc_free_mismatch-FIX-03（错误处理路径补全）
  - alloc_free_mismatch-RULE-04（循环迭代配对）→ 修复模式 alloc_free_mismatch-FIX-01
  - alloc_free_mismatch-RULE-05（动态 buffer 监控）→ 用于运行时定位，结合 alloc_free_mismatch-FIX-01/02/03/04 修复
