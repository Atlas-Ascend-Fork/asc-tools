---
name: tool-aicore-debug
description: "昇腾 NPU AI Core 异常检测与修复技能：diagnose 主入口按错误证据（错误码/报错原文/退出码/关键词）路由到运行时检测器，输出候选原因与修复方案，支持源码静态预防扫描与自动修复；lint 维护入口校验知识库一致性。触发：EZ9999 报错（stderr 含 errcode 字段）、AI Core Error、带 AI Core 异常证据的进程退出 exit_code=1、kernel 挂起或超时被 kill（exit_code=124 且 stderr 为空）、UB/L1/L2/GM 越界或溢出、L0A/L0B/L0C 读写冲突或回绕、DataCopy/DataCopyPad 对齐或 burst 配置非法、LoadData/LoadData3D 参数非法、Cube/MMAD/FixPipe 异常、SetFlag/WaitFlag、EnQue/DeQue、AllocTensor/FreeTensor 不配对、除零、负数开方、NaN/Inf、算子上板前预防性检查、detector 知识库 lint 校验。不触发：Host 侧 API/runtime 报错、算子编译失败、精度不达标（rtol/atol）、性能采集与调优。"
license: CANN-2.0
argument-hint: "[diagnose: error_code|error_str|exit_code|keyword 至少一项，或仅 source_code_path + action_mode=apply|reference + top_k? + detail=brief|full] [lint: validation_mode=full|incremental|regression + changed_seeds?]"
---

# NPU AI Core 异常检测-修复

## 两个入口

两个入口共享 `references/` 知识库；目录职责见文末，校验基线见 `references/constraints/mechanical_checks.md`。

| 入口 | 触发 | 动作 | 输出 |
|------|------|------|------|
| diagnose | 主动（用户调用） | 读 + 执行 + 改源码 | Step 0 静态预防扫描 + top-K runtime 检测-修复报告 |
| lint | 主动（维护者触发） | 只读 + 报告 | 按校验清单输出检查结果与来源统计 |

## 输入参数

### diagnose 参数

| 参数 | 类型 | 必填 | 用途 |
|------|------|------|------|
| error_code | int | 线索之一 | EZ9999 errcode（从 stderr errcode 字段提取，如 141） |
| error_str | string | 线索之一 | EZ9999 报错原文（自动抽取关键词路由） |
| exit_code | int | 线索之一 | 进程退出码：1=异常退出（须另有 AI Core 证据）、124=超时、0=正常退出；退出码本身不是硬件错误码 |
| keyword | string | 线索之一 | 直接指定关键词（覆盖自动抽取） |
| source_code_path | string | 可选 | 待检测代码文件/文件夹路径；执行范围见下表 |
| action_mode | enum | 可选 | `apply`（默认）/ `reference`，行为见 §输出 |
| top_k | int | 可选 | 正整数，默认 min(3, len(C))；非正数或非整数报输入错误 |
| detail | enum | 可选 | `brief` / `full`（默认）；执行范围见下表 |

### lint 参数

| 参数 | 类型 | 必填 | 用途 |
|------|------|------|------|
| validation_mode | enum | lint | `full`（默认）/ `incremental` / `regression` |
| changed_seeds | string | lint incremental | 变更种子列表 |

`error_code` / `error_str` / `exit_code` / `keyword` 至少提供其一（或提供 `source_code_path` 走纯静态预防扫描），否则无法路由。

### 执行范围与降级

先按下表确定有效模式，再执行适用步骤；路由优先级统一见 Step 1，写文件与修改源码行为统一见 §输出。

| 条件 | 执行范围或降级行为 |
|------|--------------------|
| `detail=brief` | 仅返回候选 ID 与摘要，不扫描源码、不评分；跳过 Step 0/2 |
| 未提供 `source_code_path` | 降为 brief；说明“缺少源码路径，未执行规则匹配”；`apply` 同时降为 `reference` |
| `detail=full` 且提供 `source_code_path` | 执行 Step 0；有运行时线索时继续 Step 1–3，路径指向的文件或目录为匹配对象，上下文代码片段可补充比对 |
| `detail=full` 且仅提供 `source_code_path`，无运行时线索 | 走纯静态预防扫描，输出 Step 0 结果 |
| 需要运行时诊断，但线索不足以定位 | 若已连接且支持，使用 CANNBOT 的 `msaicerr-toolkit` 收集 info.txt、Dump 的 .bin/.npy、Device 日志及环境信息，再回到 Step 1；工具不可用时报告缺少的证据，返回待验证候选，不声称已调用或已确诊 |

## diagnose 执行流程

### Step 0：静态预防扫描

按“执行范围与降级”表决定是否扫描；遍历 `references/detectors/static_preventive/` 并按性质分流。排序：预防型命中 > 信任型说明 > 索引型（仅查询）> 探索型（仅查询）。

| 性质 | 文档 | 动作 | 输出 |
|------|------|------|------|
| 预防型 | cube_satmode_clean / arch_api_guard / load3d_mte_param_check / fixpipe_param_check / ccu_stack_check | 执行规则匹配（值域判断 / 存在性检查） | 已确认 RULE 命中 + 保持语义的修复方案；证据或安全修复不足时只返回验证建议 |
| 信任型 | compiler_guaranteed | 不匹配；用户代码出现相关 API 模式时输出说明 | 说明性提示，无 code_blocks |
| 索引型 | ecc_ras / system_bus | 不扫源码；仅 keyword 命中时返回 | EventID 索引 |
| 探索型 | research_items / ambiguous_items | 不匹配；仅显式查询时返回 | 候选方向 |

索引型/探索型在跳过源码扫描时仍可按上表的查询条件返回。

### Step 1：运行时路由

先规范化错误证据：未显式提供 `error_code` 时，从 `error_str` 的独立十进制标量 `errcode` 字段提取；不得把 retCode、aic error mask、十六进制位图或三元组转换成十进制索引码。显式值与原文冲突时保留显式值并在结果说明冲突，不能静默覆盖。纯文本无法提取标量时继续关键词路由。仅 exit_code=1 不足以认定 AI Core 异常，应报告缺少 stderr 等证据；exit_code=0 且无其他异常线索时不创建 runtime 候选，有源码则仍可静态预防检查。

三种场景：**EZ9999 异常退出**（error_code/error_str，exit=1，stderr 含 EZ9999 + errcode）按错误码/关键词路由；**挂起超时**（exit_code=124，stderr 完全为空）路由到同步类 detector；**已知关键词查询**按 keyword 路由。按优先级：

以下分支按顺序判断，命中后不再进入后续分支：

1. **`error_code=41` 且 `exit_code=124`** → 优先路由到 `padding_misconfig`，标记 `kernel_may_hang=true`。
2. **`exit_code=124` 且 `error_str` 非空** → 按报错文本关键词查 `references/indices/keyword_index.md`，不使用纯挂起路由。
3. **`exit_code=124` 且 `error_str` 为空** → 挂起型路由 [flag_mismatch, alloc_free_mismatch, enque_deque_mismatch, padding_misconfig]；与显式 `keyword` 取交集。
4. **`error_code` 提供且匹配** → 查 `references/indices/error_code_index.md`（含碰撞展开，如 141→[fixpipe_gm_overflow, atomic_fixpipe_overflow]；124→[datacopypad_ub_overflow, mte_write_ub_overflow]；14/39→l0b_rdwr_conflict）。
5. **`error_code` 缺失或未匹配索引** → 用显式 `keyword`，否则从 `error_str` 抽关键词查 `references/indices/keyword_index.md`；未知错误码须注明“错误码未收录，已回退关键词”。无可用关键词或匹配为空时返回空候选并说明证据不足，不把未知错误码当作正常退出。

关键词路由中，用户显式提供的 `keyword` 覆盖自动抽取的关键词；不覆盖已命中的错误码分支。输出候选 ID 列表 C。

> **exit_code 与 error_code 的区别**：`error_code` 是 EZ9999 stderr 中的 errcode 字段值（如 141、124）；`exit_code` 是操作系统进程退出码（0/1/124）。两者独立，不存在转换关系。`error_code=124`（UB 溢出，exit=1，stderr 非空）与 `exit_code=124`（超时挂起，无 stderr）是完全不同的异常。
>
> **padding_misconfig 双路由**：同时挂在错误码与挂起索引下，组合输入按上述第 1 分支处理；异常表现详见 `references/detectors/runtime/sync_hang/padding_misconfig.md`。

### Step 2：规则匹配（runtime/ detectors）

对每个 c ∈ C，读 `references/detectors/runtime/{category}/{c}.md` 的"检测执行指引"段，按"适用匹配模式 / 命中判定 / 修复映射"三个字段逐条匹配。

- 按“执行范围与降级”表确定源码输入；路径指向目录时，递归扫描 `.cpp`/`.asc`/`.h`/`.hpp`/`.hxx`/`.cuh`。跟随工程内可解析的 include、参数定义和调用关系补充上下文，头文件与 include 重复引用同一规则证据只计一次；工程外依赖或编译宏无法解析时标明分析范围和限制。
- 容量、单位、历史种子适用范围、辅助规则与硬件归因统一遵循 `references/constraints/semantic_contract.md`。仅缺少断言或命中通用 OOB 兜底不能确诊当前异常；不确定的修复仅作为验证建议。
- 匹配模式及字段含义见 `references/templates/detector_template.md`。
- 判定受限时，在该规则的"命中判定"中按需标注：值域追溯超过 3 层或跨越函数 / 方法边界而无法定值时记 `trace_limited=true`；安全条件依赖静态无法确定的运行时值或尚未取得的运行证据时记 `runtime_required=true`。两者可同时出现；即使 detector 未预写这些可选标注，执行时仍须按实际证据判定。现有输入已补足所需证据时，正常判定并计分，不因文档预写标注而排除该规则。
- 每候选记 `rule_hit_rate` = 已确认命中根因规则数 / 可判定根因规则数。先按语义约定排除辅助规则，同一违规证据的等价根因规则合并为一项，再扣除追溯受限或待运行时验证的根因规则（同一规则只扣一次）；这些规则均不计入分子和分母，并在 `fix.reason` 中注明排除依据、"追溯受限"或"需要运行时验证"。分母为 0 时记为未评分（`rule_hit_rate=null`），保留候选并说明证据不足。
- 可选补强：需要补充跨异常依据时，读取 `references/indices/cross_references.md` 的"共享检测规则"，按适用异常 ID 与候选证据比对，结论可写入 `fix.reason`；共享规则不额外计入 `rule_hit_rate` 的分子或分母，与自身规则重叠时仅按自身规则计分。
- 同步挂起型候选（flag/alloc_free/enque_deque）以 **API 调用模式**为主：按队列、事件和实际控制流路径检查配对，不能只比较源码中的调用次数。`padding_misconfig` 按 padding/fmap/filter 值域和组合约束检查，不属于 API 配对规则。

### Step 3：排序 + 输出

已评分候选按 `rule_hit_rate` 降序排列，同分保留路由顺序；未评分候选排在其后并保留路由顺序，再取 top-K（默认 `K = min(3, len(C))`；指定时 `K = min(top_k, len(C))`），按 `action_mode` 分流（见 §输出）。top-K 只限制 runtime 候选，Step 0 的静态结果按其自身顺序放在 runtime 结果前，不占 runtime 名额。

## lint 执行流程

输入参数见 §输入参数；按 `references/constraints/validation_rules.md` 选择校验范围。检查定义、编号、失败处置及统计口径统一以 `references/constraints/mechanical_checks.md` 为准，按其编号顺序输出报告。lint 不使用 diagnose 的 JSON/Markdown 报告模板。

由模型读取所选范围内的文档，逐项核对 ID 集合、索引、字段、规则及来源统计，直接生成检查报告。集合表达式和正则仅描述判定标准，不要求创建或运行额外检查脚本。未能读取、核实或统计的项目须注明“未验证”及原因，不得推定通过；模型检查不等于已通过实际 YAML 解析、编译或设备运行验证。

## 输出

### action_mode 分流

| action_mode | 改源码 | 写结果文件 | 对话返回 |
|-------------|--------|-----------|---------|
| apply（默认） | ✓ 仅对已确认根因、匹配当前源码且不改变目标语义的修复执行 in-place 替换 | ✓ `cann-output/result-{ts}.json` + `.md` | 简短变更摘要 + 文件路径 |
| reference | ✗ | ✓ 同上 | 文件路径 + 一句话摘要 |

- `cann-output/` 不存在时自动创建；timestamp 格式 `YYYYMMDD_HHMMSS`（如 `20260713_143025`）
- brief 不修改源码；`apply` 降为 `reference`。候选未确认、before 不唯一或修复可能改变算子语义时，不执行替换，在 `fix.reason` 说明；互斥候选不能同时应用到同一代码段。优先遵循用户对工具调用和写文件的明确限制。
- 报告字段与 JSON → Markdown 渲染规则统一见 `references/templates/output_spec.md`；`output_json.md` 提供 JSON 示例，`output_md.md` 提供 Markdown 骨架。两种 action_mode 均按该契约生成报告。

## 目录结构

| 路径 | 职责 |
|------|------|
| `SKILL.md`（本文件） | diagnose + lint 双入口门面 |
| `references/` | 知识库根目录（detectors / indices / constraints / templates） |
| `references/detectors/runtime/{category}/` | 运行时检测器；ID 与类别见 `references/indices/seed_mapping.md` |
| `references/detectors/static_preventive/` | 静态预防与说明文档；性质分流见 Step 0 |
| `references/indices/` | 索引：`seed_mapping.md`、`error_code_index.md`（含 exit_code 索引）、`keyword_index.md`、`cross_references.md` |
| `references/constraints/` | `mechanical_checks.md` 定义检查；`validation_rules.md` 定义执行范围 |
| `references/constraints/semantic_contract.md` | 容量/单位、辅助规则、证据与修复约定 |
| `references/templates/` | 检测器结构定义、报告契约及输出示例/骨架 |
| `evals/evals.json` | 技能评估用例（diagnose 路由 / 挂起型 / 静态预防 / lint 四类） |

> **路径约定**：本文及知识库内所有 `references/...` 路径均相对技能根目录解析。

## 评估用例检查

使用 `evals/evals.json` 时，在独立上下文中向模型提供用例 `prompt` 和必要的技能知识，生成回答后再由模型对照 `expected_output` 与 `expectations` 评审。`contains` / `not_contains` 仅作为文本辅助检查，不能替代语义判断；尤其不能仅凭出现数字就认定公式或结论正确。技能是否实际激活、是否执行路由须依据可用的执行轨迹，缺少轨迹时标注“未验证”。整个评审流程不依赖额外 Python 脚本。
