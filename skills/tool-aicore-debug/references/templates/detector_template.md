# detector 合并文档骨架模板

本模板是 `references/detectors/runtime/{category}/{ID}.md` 与 `references/detectors/static_preventive/{ID}.md` 合并文档的统一骨架，供人工新增 detector 时复用。八段落缺一不可；检测规则每条必含四标记字段。

> **路径约定**：runtime detector 存放于 `references/detectors/runtime/{category}/{ID}.md`（9 类之一）；静态预防 detector 存放于 `references/detectors/static_preventive/{ID}.md`。

---

## 字段定义与填写说明

### 八大必填段落（外加一级标题）

每个 runtime detector 必须含以下段落（外加一级标题 `# {ID} — {异常名称}`）：

1. **基本信息**（表格：异常编号 / 异常类型 / 错误码 / 类别 / 源文件 / 报错模式 / 内存区域 / kernel_diff_type / behavior_note）
2. **触发条件**（最小触发条件，描述违规模式与安全模式的差异）
3. **检测规则**（`### {ID}-RULE-{NN}` 编号；已有的 DEPTH/TILING 等助记后缀也可保留，完整规则 ID 在文内唯一；每条规则含四标记字段）
4. **修复方案**（标准修复模式 + seed 具体修复 + 代码模板 + 防御性编程建议）
5. **变体索引**（表格：变体 / 路径 / 触发方式 / 等价类）
6. **等价类划分**（变异维度 + 等价类表）
7. **关联异常**（共享参数 / 传播链 / 症状相似）
8. **检测执行指引**（供 diagnose Step 2 直接执行：适用匹配模式 / 命中判定 / 修复映射三个非空字段；`trace_limited` / `runtime_required` 可按需在命中判定内标注）

### 四标记字段（每条检测规则必须同时含）

- **【条件】** 触发异常的代码模式或运行时条件
- **【API】** 规则涉及的 API 名称（用于按 API 过滤候选）
- **【约束】** 安全条件（即不触发异常的条件，用于与用户代码对比）
- **【匹配模式】** 取值：`变量名匹配` / `值域判断` / `API 调用模式` / `存在性检查` / `diff_pattern`（五选一，决定 diagnose Step 2 的执行动作）

正式异常编号必须与文件名一致，硬件 L 编号等另存 `硬件事件编号`；`kernel_diff_type` 与 seed_mapping 枚举保持一致，具体变更可存 `kernel_diff_note`。来源字段可选，但缺失必须在 lint 来源统计中计入“未标注”。辅助规则、容量与单位的解释统一见 `references/constraints/semantic_contract.md`，不能仅按变量名、缺断言或 OOB 兜底确诊异常。

### 匹配模式说明（5 类）

| 匹配模式 | 适用 |
|---------|------|
| 变量名匹配 | 检查特定变量名是否出现（如 tiling 参数字段） |
| 值域判断 | 数值范围 / 阈值比较（如 offset + length > BUFFER_SIZE） |
| API 调用模式 | API 调用序列配对（如 SetFlag/WaitFlag、AllocTensor/FreeTensor） |
| 存在性检查 | 某代码模式是否存在（如 `(1ULL << 48)` 位设置） |
| diff_pattern | 描述违规→安全的 diff 模式（error/fixed 行级差异，保留 diff 语义） |

---

## 骨架正文 — runtime detector（填写模板）

# {ID} — {异常名称}

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | {ID} |
| 异常类型 | {error_type} |
| 错误码 | {error_code 或 "无（kernel 挂起型）" 或 "无（EZ9999 无数值型码）"} |
| 类别 | {runtime category slug 之一} |
| 源文件 | {来源种子目录路径} |
| 报错模式 | {EZ9999_stderr / kernel_hangs / 无明确报错} |
| 内存区域 | {UB/GM/L1/L0_A/L0_B/CUBE_OUT/多区域} |
| kernel_diff_type | {kernel_diff / host_only_diff / no_diff} |
| behavior_note | {特殊行为备注，无则 "-"} |

## 触发条件

{最小触发条件，描述违规模式与安全模式的差异}

## 检测规则

### {ID}-RULE-01：{规则名}

【条件】...
【API】...
【约束】...
【匹配模式】...

匹配示例：...

（重复 N 条，每条规则编号递增 {ID}-RULE-02、{ID}-RULE-03 ……）

## 修复方案

### 标准修复模式

1. ...

### seed 具体修复（可选，有种子的 ID 必填）

历史种子 error→fixed 的具体 diff 摘要（违规写法 → 安全写法）。

### 代码模板

...

### 防御性编程建议

...

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| ... | ... | ... | ... |

## 等价类划分

变异维度 + 等价类表

## 关联异常

- 共享参数 / 传播链 / 症状相似

## 检测执行指引

- **适用匹配模式**：{列出本 ID 使用的合法匹配模式及对应 RULE 编号}
- **命中判定**：{逐条说明证据提取、约束比较与命中条件；保留必要的种子复核步骤和故障区分依据}
  - {可选：说明本规则何时追溯受限，标注 `trace_limited=true`；或何时依赖运行时证据，标注 `runtime_required=true`。统一判定与计分口径见 `SKILL.md` diagnose Step 2。}
- **修复映射**：{逐条说明 RULE 命中时对应的修复模式或下一步验证动作}

三字段均必填，正文可写在冒号后或其下缩进的子列表中。两种判定标注仅在适用时写入"命中判定"，不设独立必填字段；未写标注不表示执行时可以将证据不足的规则计为未命中。

---

## 骨架变体 — static_preventive detector（4 种性质）

静态预防 detector 存放于 `references/detectors/static_preventive/{ID}.md`，按**性质**有 4 种变体。基本信息表追加 `类别=static_preventive` 与 `性质` 字段。

### 变体 A：预防型（标准，含修复方案 + code_blocks）

适用于"硬件不检出 / 架构限制"类（cube_satmode_clean / arch_api_guard / load3d_mte_param_check / fixpipe_param_check / ccu_stack_check）。继承 runtime 8 段骨架，但基本信息表调整：

| 属性 | 值 |
|------|---|
| 异常编号 | {ID} |
| 异常类型 | 静态预防（硬件不检出 / 架构限制） |
| 错误码 | 无（硬件静默） |
| 类别 | static_preventive |
| 性质 | 预防型 |
| 覆盖条目 | {AICoreError.md 文档行号，如 L38/L39/L40} |
| 检测时机 | host tiling 计算 / kernel 编译期 |
| 报错模式 | 无（硬件不报，需应用层预防） |

检测规则以值域判断 / 存在性检查为主，修复方案含 host 侧 tiling 断言 / 编译期 static_assert / 防御性代码模板（before/after code_blocks）。

### 变体 B：信任型（简化，无 code_blocks）

适用于"编译器/驱动保证"类（compiler_guaranteed）。基本信息表 `性质=信任型`，**不含 code_blocks**：

- 段落精简：基本信息 + 触发条件 + 编译器保证依据 + 用户行动建议 + 关联异常 + 检测执行指引
- 检测执行指引标注"无规则匹配，仅说明性文档"
- 输出形态：说明性提示，无 before/after code_blocks

### 变体 C：索引型（仅 EventID + 处理方式）

适用于"应用层无法处理"类（ecc_ras / system_bus）。基本信息表 `性质=索引型`，**不扫源码**：

- 段落精简：基本信息 + EventID 索引表 + 应用层建议 + 检测执行指引
- 检测执行指引标注"无规则匹配，仅索引性文档，不扫源码"
- 仅在 keyword 命中时返回（diagnose Step 0 不主动输出）

### 变体 D：探索型（未确认，标注候选方向）

适用于"未确认"类（research_items / ambiguous_items）。基本信息表 `性质=探索型`，追加 `状态=未确认`：

- 段落精简：基本信息 + 候选检测方向 + 检测执行指引
- 检测执行指引标注"未确认条目，仅记录候选方向，不参与规则匹配"
- 仅在显式查询时返回
