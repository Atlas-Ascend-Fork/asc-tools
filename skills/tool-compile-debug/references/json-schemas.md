# 诊断与验证结果 JSON 字段

当其他调用方或集成方需要稳定的输出字段时，使用本参考。

## v2模式与证据范围

`diagnose`摘要包含`schema_version=2.0`、`context_mode`、`observation_status`及`log_evidence`。观察状态为failed或unknown，日志本身不能证明执行成功。`log_evidence`给内容hash、行数和完整性未断言；错误的`log_line`与`original_log_span`指原始日志位置，错误家族的`occurrences`保留每个发生位置。

默认light只产出日志解析、轻量诊断、空源码上下文、未请求的环境、空建议、报告和日志副本。`env.cann.found=null`与环境检查unknown表示未探测，不是工具链缺失。下文完整目录示例适用于full，不能要求light具有根因树、静态检查或可复用源码快照。focused也不提供完整失败源码身份。只有带适用源码/配方身份的失败证据才能支持`compile_failure_fixed`比较。

`inspect`仍为schema_version=2.0，新增`operation_status`与`evidence`。`operation_status=completed`只表示查询完成；`status`区分completed/partial/no_match/error。`evidence`给binding（selected/ambiguous/unbound/not_requested）、coverage、freshness（unknown/not_applicable）、semantics及acceptance=not_assessed。完整文件含gaps，stdout用gap_count/gap_reasons及source_reads/metadata_reads计量代替详细内容；消费者需要逐文件hash时读取`--out`，不依赖旧stdout内嵌完整read_evidence。

source的read_evidence给源码读取数、字节、hash与gaps；metadata_read_evidence单独计量已有构建元数据。缺失/越界文件、预算不足、无编译记录、未绑定/歧义配置、相关命令无法重放为partial，不能解读为不存在相关定义。configuration_id只绑定已有命令，不能证明数据库最新。多个定义保留definition_candidates与ambiguities，semantics=lexical_candidates不选任意一个作为生效定义。

构建记录包含original_arguments、arguments、invocation_id、configuration_id、response_files（路径/hash/bytes）及response_expansion（not_needed/complete/partial、语法、预算、gaps）。complete只表示受支持语法已展开。含响应文件时configuration_id绑定展开内容和所读响应文件身份；文件缺失/可用或内容变化会失效旧选择器，须重新查询。build_facts.response_token_budget与response_byte_budget记录整次查询的展开余量；metadata_read_evidence.allowed_roots列出实际读取范围。外置生成文件以绝对路径为key。完整元数据账本保留所有读取缺口，顶层evidence只采纳选中命令的响应文件缺口；无关命令失败不冒充当前目标失败。

knowledge按符号返回api.query、candidates、matching_symbol_count、candidate_count、filtered_count、omitted_count、version_resolution及applicability_reasons。完整过滤发生在limit前；省略候选不会消除重载歧义。absence_proven固定false，查询条件不是经过编译器核实的调用事实。机制卡与API候选都不能直接作为修复已验证的结论。

`verify --observe-run`输出独立执行收据：schema_version、run_id、identity、argv、cwd、source、environment_sha256、status、returncode、log_path、log_sha256、log_capture及时间。环境只保存摘要。观察核请求、终态身份和日志一致性；unknown或interrupted不重派发。独立passed不是修复结论，完整verify的覆盖与validation_scope仍不可省略。

`verify --recover-verification DIR --json-out FILE`复用完整verify的结果口径，并给`recovery.directory/status/gaps/new_build_invoked`。`collected`是收取状态，不能当作编译通过；适用检查通过另给`current_applicability=matched`。缺口返回`transition=verification_incomplete`、`validation_scope.compile=unconfirmed`，已有历史报告仅在`historical_result`保留。调用方不得从历史字段提升当前结论。顶层`build_invoked`描述原运行，本次恢复不会新增构建。持久记录以`schema_version=1.0`、`sha256`和`payload`封装，执行请求仍沿用2.0；不是另一套案例评测schema。具体材料、限额及支持边界见[恢复契约](repair-state.md)。

`build_invoked`采用三值：`true`有启动证据，`false`确认未启动，`null`证据不足。派发后汇总/存储异常为`verification_incomplete`，不能按CLI异常改写为未执行；`recovery.executions`保留已有运行身份。不得把`null`强转布尔后计入“未消耗机会”。正常构建失败仍可返回完整报告；CLI自身异常退出1，外部结果文件写入失败另输出stderr恢复指针。

可选复杂任务状态为`repair-memory-v1`：`state_id`校验快照内容，`parent_id/sequence`连接历史，`facts`绑定文件/配置和依赖身份，`hypotheses/obligations/collaborations`保存宿主陈述，`attempts`记录可复核动作。`current/stale`仅表明依赖身份，`acceptance`恒为`not_assessed`。机制查询的`investigation`提供缺失证据角色、问题、探针与不同结果的下一动作；不会自动执行或将宿主判断升级为验证结论。详见[证据状态](evidence-memory.md)。

## 错误契约

入口不保证所有故障都返回同一 JSON：

- `diagnose` 在参数解析成功、已进入诊断执行且输出目录可写时，捕获的诊断异常写入 `diagnosis.json` 并以 1 退出；带 `--json-stdout` 时摘要含 `error`，否则 stdout 给出错误产物路径。
- `verify` 自身异常退出1；报告可写时保存`error`、`recovery`和未确认的验证范围。仅确认未启动时为`build_invoked=false`、`transition=verification_not_run`；已启动或启动状态未知时保留`true`或`null`，使用`verification_incomplete`。正常完成的构建失败属于有效结果，CLI可以退出0，须读取`after.returncode`、`transition`及覆盖证据；不能把入口状态当作编译结论。
- `redact` 成功时输出统计或恢复信息；缺输入、口令或依赖等失败可能只在 stderr 报错，不承诺 `error` JSON。
- 包装器的参数、平台、安装目录保护、依赖导入，以及结果写入自身失败，都可能发生在上述产物生成之前。调用方须一并保存退出码、stderr 和完整 stdout；没有 JSON 时先处理入口或写入故障。

诊断或验证产物中的 `error` 对象示意如下；`stage` 与附加上下文随入口而变：

```json
{
  "error": {
    "type": "ValueError",
    "message": "human readable message",
    "stage": "skill_name_or_stage",
    "summary": "ValueError: human readable message"
  }
}
```

## 产物目录结构

```text
diagnosis_bundle/
  env.json
  normalized.json
  context.json
  static_findings.json
  probes.json
  diagnosis.json
  failure_evidence.json      # full含源码身份；focused仅日志证据，light不生成
  cause_tree.json            # 仅当 diagnosis.cause_tree 为对象时
  verify.json                # 仅当诊断期带 --verify 时
  patch_suggestions.json
  report.md
  logs/build.log
```

## `--json-stdout` 摘要（★ 调用方实际读取面）

`diagnose_compile_failure.sh ... --json-stdout` 打到 stdout 的载荷。**这是调用方决策所依据的摘要结构**（需要时按 bundle/files 字段取得完整证据）。字段与 `diagnose_compile_failure` CLI 使用同一个 `build_stdout_summary`；具体路径与可用证据随调用输入而变。完整产物内的 SDK 声明 note 与 root hint 按原日志路径（有 `file_original` 时优先）关联；展示用短路径不能证明同源，外部声明也不因此获得工程归属或旧失败覆盖证明。

```json
{
  "bundle": "workspace/reports/diagnosis_bundle",
  "summary": "...",
  "phase": "kernel_compile",
  "category": "api_incompatible",
  "primary_error": { "message": "...", "file": "...", "line": 38, "category": "..." },
  "root_cause_localization": { "root_file": "...", "root_symbol": "...", "evidence": {} },
  "self_explanatory": true,
  "fast_path": {
    "kind": "compiler_suggested_rename",
    "confidence": "high",
    "action": "把 <file> 第 38 行的 `AscendCC` 改成 `AscendC`",
    "replace": { "from": "AscendCC", "to": "AscendC", "file": "...", "line": 38 },
    "then": "改完调 run_verify(case_dir=<project_dir>) 真实编译确认",
    "do_not": "建议文本；不能禁止为检验当前假设所需的取证"
  },
  "kb_evidence": [
    { "api_name": "Mins", "signature": "...", "header": "...",
      "evidence": "kb_affix_exact", "notes": "..." }
  ],
  "next_actions": [],
  "top_suggestion": {
    "rank": 1, "type": "diagnostic_guidance", "title": "对照调用实参与编译器候选重载",
    "patch": "", "risk": "unknown", "mechanism": "overload_resolution_failure",
    "applicability": "conditional", "action": "按实际声明和用途核参数数量与类型，再决定修调用或声明。",
    "compiler_notes": [], "verification": "沿原工程、目标和完整构建参数运行run_verify。"
  },
  "version_compatibility": {
    "kb_baseline": "9.0.0",
    "kb_floor": "9.0.0",
    "ceiling": "9.1.0",
    "supported_domains": [[9, 0], [9, 1]],
    "detected": "9.0.0 | 8.3.RC1 | {\"log\": ..., \"env\": ...} | null",
    "source": "log | env | log+env | log+env-conflict | none",
    "status": "match | mismatch | unknown",
    "resolution": "supported | ahead | below_floor | conflict_in_domain | unknown",
    "conflict_in_domain": false,
    "note": "可选；补充说明未知版本、超出范围或日志与环境版本冲突"
  },
  "verify": {},
  "run_id": "...",
  "files": { "diagnosis": "...", "report": "..." }
}
```

`version_compatibility`（版本域门控）：随包知识的版本域由登记表驱动（基线 `kb_baseline`，已核实域见 `supported_domains`）。必须结合 `status` 与 `resolution`；`match` 可对应 `ahead` 或域内版本冲突，不能等同于当前版本已核实，`note` 在这些状态下也可能存在。`status == "mismatch"` 时 fast_path 已被抑制、`next_actions` 提示核版本与目标；按已有用户意图继续，实际未决意图才确认。kb_evidence / top_suggestion 保留为参考；`unknown` 表示 log/env 均无可信版本证词（未展开的 `$CANN_VERSION` 占位符不算版本）。`run_verify` 的返回同样带该字段；mismatch 时另有 `version_mismatch_note`（编译事实照报，版本结论仅对已核版本域语义负责）。

字段语义与逐个处置见 [using-scripts.md](using-scripts.md)（唯一权威调用契约）。要点：

| 字段 | 取值 / 语义 |
|---|---|
| `env.npu_probe_status` / `env.npu_devices` | 默认 `not_requested` / null，表示诊断未探测设备，不能解释为无 NPU。`observed` 时计数来自已解析的设备记录，不证明完整设备清单或可运行性；工具不可用、查询失败、无匹配记录或无证据时计数仍为 null。本技能是否需要运行回归由补丁风险决定，不能据此禁止；原任务独立的运行验收交回调用方。 |
| `fast_path.kind` | `compiler_suggested_rename`（did-you-mean候选）／ `kb_known_symbol`（符号候选，附signature/header）／ `signature_deterministic_fix`（规则action，需核适用前提）／ `needs_localization`（核定义/调用与契约决定改哪侧）／ `ambiguous_fix`（多条规则候选同命中，附候选集，须按声明、目标与调用契约区分，不默认选边）／null；任意建议均可被源码、目标头与真实编译反证 |
| `kb_evidence[].evidence` | `kb_affix_exact`（结构等式命中）／ `api_version_map`（改名映射）／ `kb_near_name`（近似候选）；均须核当前适用性与来源，不能机械照改 |
| `self_explanatory` | `true` = 当前启发式认为编译器file+line线索可直读，未发现项目内跨文件根；不是已排除其他根因或禁止继续取证 |
| `files` | 只含 `diagnosis` + `report` 两个键（仅保留 diagnosis 与 report 两个键，其余中间产物路径已移除）|
| `error` | 仅诊断本身失败时出现；此时 `phase`/`category` 为空，**不是"未命中"** |
| `failure_stage` | value为configure/generate/driver/preprocessing/compilation/build/link/packaging/unknown，basis给证据类型；log_line位于归一化日志空间 |
| `source_context` | 当前源码的简单函数归属线索：host/device/unknown，已知时含file/line/function、source_text_sha256及binding=current_source_only；哈希是收集文本UTF-8值，不声称原文件字节或历史源码一致 |
| `diagnostic_facts` | schema_version=diagnostic-facts-v1；mechanism.value与basis、primary_log_line、log_line_space=normalized；sdk_association.status及basis独立取证 |
| `top_suggestion` | 通常为diagnostic_guidance，patch严格为空；action为条件指导，compiler_notes是同块有限摘录。历史门控建议卡/历史type仍兼容，不代表当前诊断生成diff |
| `path_hint` | value为host/device/unknown，basis=path_only；文件后缀和目录不建立实际编译/执行域 |
| `conflicts` | axes/values/reason列表；新证据与路径或旧phase不一致时保留，不能凭旧标签覆盖新证据 |
| `decision_advisory` | 可选；提示可能的用户意图分歧（如默认值与显式请求不一致），先用需求和调用关系消歧，不是裁决 |
| `unattended_default` | 可选；标记某结论源自无人值守时的默认选择，不能当作用户确认过意图 |

宏/条件指令、模板、嵌套类或方括号语法（含数组和lambda）、位置歧义、非ASCII/tab行上尚未确定单位的列号、源码未加载或重定位均可能使source_context 保持unknown，blockers说明具体缺口。空缺字段兼容旧产物/诊断故障，不代表已排除。

`sdk_association.status`四态：`not_established`（未建立归属）、`explicit_namespace_use` （失败符号明确使用SDK根namespace）、`log_bound_sdk_declaration`（日志关联原SDK同名声明）、 `not_applicable`（该语法/工具事实无需API归属）。关联声明需原路径、正行号、同诊断块及匹配符号，引用保存在`evidence_refs`；展示短路径不能代替原来源。关联不证明API存在、版本移除、目标适用或修复成功。 `normalized.json`里的note可另带`code_line`、`code_line_log_line`、`declaration_symbol`，保留受限识别的声明回显；`sym`仍只来自note消息。下一条诊断/命令不能当回显，无可靠回显就不填。 primary/note的`diagnostic_block`由当前解析器保存：可信回显/caret、空行和连续声明note可续接，其他非空输出保守断块。缺该身份或跨块时不升级SDK声明关联；旧SDK升类消费同一证据层。

## 关键输出字段

`normalized.json`:

```json
{
  "status": "failed",
  "primary_error": {
    "message": "...",
    "file": "...",
    "line": 1,
    "column": null,
    "phase": "kernel_compile",
    "category": "header_missing",
    "confidence": 0.9
  },
  "error_codes": [],
  "all_errors": [],
  "downstream_errors": [],
  "normalized_signature": "..."
}
```

`static_findings.json`:

```json
{
  "findings": [
    {
      "rule_id": "tiling_field_mismatch",
      "severity": "error",
      "category": "tiling_consistency",
      "file": "add.asc",
      "line": 83,
      "message": "...",
      "evidence": [],
      "suggested_fix": "..."
    }
  ],
  "summary": {
    "total": 1,
    "errors": 1
  },
  "contract_summary": {}
}
```

`diagnosis.json`（完整诊断；stdout summary 是它的投影 + `fast_path`/`kb_evidence`）:

```json
{
  "summary": "...",
  "phase": "kernel_compile",
  "category": "tiling_consistency",
  "primary_error": {},
  "root_cause_candidates": [],
  "root_cause_localization": null,
  "self_explanatory": false,
  "cause_tree": {},
  "is_compiler_internal_suspected": false,
  "next_actions": [],
  "probes_interpretation": {},
  "status": "failed"
}
```

`root_cause_localization`（非 null 时的完整结构，跨文件定位）:

```json
{
  "symptom_file": "add.asc",
  "root_file": "add_tiling.h",
  "root_line": 12,
  "root_symbol": "TilingData::blockLen",
  "family": "tiling_field_mismatch",
  "method": "tiling_field_def_header",
  "confidence": 0.9,
  "collapse_status": "collapsed",
  "collapsed_count": 3,
  "collapsed_files": ["add.asc", "add_host.cpp"]
}
```

`root_file` 给出可能不同于症状文件的根因候选位置；修改前仍需核对当前声明、使用方和实际编译单元。签名与架构参考证据由 stdout summary 的 `kb_evidence` 提供，不在本对象内。`collapse_status` 表示定位器是否将多个下游报错归并到同一候选根，不证明该候选已经验证。

`patch_suggestions.json`（兼容文件名；当前保存条件指导，不生成diff）:

```json
{
  "schema_version": "compile-repair-guidance-v1",
  "suggestions": [
    {
      "rank": 1,
      "type": "diagnostic_guidance",
      "title": "对照调用实参与编译器候选重载",
      "risk": "unknown",
      "patch": "",
      "mechanism": "overload_resolution_failure",
      "applicability": "conditional",
      "action": "核候选声明、实际参数及原接口，证据支持时再修调用或声明。",
      "compiler_notes": [
        {
          "kind": "compiler_note", "log_line": 4, "log_line_space": "normalized",
          "file_original": "/toolchain/include/vector.h", "line": 106,
          "diagnostic": "candidate function not viable: requires 4 arguments, but 5 were provided",
          "binding": "same_diagnostic_block"
        }
      ],
      "verification": "沿原工程、目标及完整构建参数运行run_verify；核实际编译与补丁契约。"
    }
  ]
}
```

上例为字段示意，note路径本身不证明SDK归属。`compiler_notes`最多三项；可选`source_excerpt`与`declaration_symbol`只描述已加载的日志摘录。 diagnostic最多360字符、source_excerpt最多240字符，均可能不完整，不能当完整可调用声明；需要时沿原位置读完整normalized日志。声明归属、目标适用与补丁正确性仍分别验证。没有首错或no_compile_error时suggestions为空。`schema_version`属于本文件的顶层，不另加到top_suggestion项中。新type必须patch为空；历史带diff的旧文件仍可用于显式补丁验证。

## 验证摘要

正常验证在stderr输出 `[verify]` 加一行JSON；`python3 "$SKILL_DIR/scripts/run.py" verify --summary "$RESULT_JSON"` 只读已有完整报告，在stdout输出同一结构，不重新构建或更新报告。两者的 `schema_version` 都是 `verify-summary-v1`。摘要保持原判定，缺失字段为null；类型不兼容时明确报错，不把旧报告缺证据解释为通过。

| 摘要字段 | 来源与含义 |
|---|---|
| `result_path` | 本次读取的完整JSON绝对路径；仍将该原JSON用于前态复用 |
| `transition` / `reason` | 原报告原值，未知transition也不推断成功 |
| `build_returncode` / `build_invoked` / `dry_run` | 构建返回码及是否实际构建；不能单独代替验收 |
| `built_target` / `expected_target` / `built_arch` | 原值，允许字符串、字符串列表或null；逐项对应用户原目标 |
| `compilation_evidence` / `compilation_reason` | 完整报告 `compilation_evidence.status` / `.reason`；摘要的前者保留旧接口的状态字符串类型 |
| `target_validation` | 只含 `status`、`requested_arch`、`observed_architectures` |
| `prior_failure_coverage` | 来自完整报告 `target_validation.prior_failure_coverage`；保留 `status`、`reason`，另给四种文件列表的数量 |
| `contract_check` | 原 `status` / `reason`；`violated` 不因编译通过而隐藏 |
| `escape_suspect` | 可空告警对象；存在时仅含原 `check`、`evidence_log` 与 `missing_symbols_count`，不展开符号列表和advice；缺席为null，不表示检查完成或无疑点 |
| `validation_scope` | 原 `compile` / `contract` / `numerical`，未核验保持 `not_checked` 或null |
| `error` | 原异常的 `type`、`stage`、`message`；message最多500字符，`message_truncated` 标记是否截断，不展开details |
| `log_path` / `recipe_cwd` | `after.log_path`（缺省时 `logs.after`）及 `after.recipe_cwd`；使用实际路径，不猜日志名 |

覆盖数量键是 `required_files_count`、`missing_files_count`、`unresolved_files_count`、`reanchored_files_count`。对应列表存在且为空才为0，列表缺失或null则为null；数量不等于成功编译单元数，也不能替代原文件身份及覆盖判据。摘要不展开 `records`、源码/依赖哈希、完整日志或失败前态。

例如，以下状态可以同时成立，摘要必须原样保留：`build_returncode=0` 且 `validation_scope.compile="unconfirmed"`；`transition="compile_passed"` 且 `contract_check.status="violated"`；或 `compile_failure_fixed`、契约 `ok` 同时带非null的 `escape_suspect`。符号缺失告警需要结合原日志和最终源码另行核验，不自动改变原结论，也不能因其它轴通过而隐藏。`missing_symbols_count` 仅在原列表存在时计数，缺列表为null。`numerical="not_checked"` 不因存在编译回执而升级。读取摘要的退出0仅表示读取成功，完整参数与错误处理见[脚本契约](using-scripts.md#结果短读与产物复用)。

## 完整验证结果

`verify.json`（`scripts/run_verify.sh`的输出示例；本例是有真实before构建记录的patch模式，不能套造就地模式前态）:

```json
{
  "before": {
    "status": "failed",
    "signature": "...",
    "returncode": 2,
    "log_path": "..."
  },
  "after": {
    "status": "passed",
    "signature": null,
    "returncode": 0,
    "log_path": "..."
  },
  "transition": "compile_failure_fixed",
  "verify_mode": "patch",
  "build_invoked": true,
  "dry_run": false,
  "patch_apply": {},
  "workspace": "workspace/builds/verify/<case_id>",
  "logs": {
    "before": "...",
    "after": "..."
  }
}
```

判读三个关键字段：

- **`transition`** —— 结论。各状态的逐一处置见
[using-scripts.md](using-scripts.md)。两种compile成功状态都确认本次编译；fixed另需适用失败前态和必要源码覆盖。原接口与功能契约仍须保持；只有具体补丁风险需要时才补相关回归证据，不固定要求数值测试或NPU运行。
- **`verify_mode`** —— `"in_place"`（只给了 `CASE_DIR`，验证你当前的工作副本，**推荐**）
或 `"patch"`（给了 `--apply-patch`，验证一份独立补丁）。
- **`build_invoked`** —— `false` 表示本次未调用真实构建，不能据此判修复成败；`true`仍须核对目标TU和依赖实际参与的证据。

成功状态按 `result["transition"]` 判断；不存在顶层 `compile_passed` 或 `compile_failure_fixed` 布尔字段。实际目标、架构读顶层 `built_target`、`built_arch`，日志读 `after.log_path` 或 `logs.after`，构建工作目录读 `after.recipe_cwd`。目标可能是字符串、列表或null，按当前结果核对原配方要求的全部目标，不把一个目标通过外推为多目标完成。

`case_not_reproducible`、`target_validation.status=unconfirmed` 或 `prior_failure_coverage.status=unconfirmed` 表示工具的覆盖证据未确认，不能按状态名推断“原错修好所以不复现”。按 `reason` 区分未执行目标、未覆盖源码和记录不足。已有日志、真实编译命令与产物能补齐证据时，保留原JSON并另述人工核验结果；目标确未执行或最终源码未覆盖时，再沿原配方补验。完整调用及处置以[脚本契约](using-scripts.md)为准。

`compilation_evidence.required_sources`列出本次应编译的源码；可选`source_scope`说明集合来源。`status=configured`、`basis=cmake_file_api`表示由独立配置清单确定目标及依赖，附`requested_target`、`target_names`和配置索引/hash；`conservative_fallback`表示缺少可靠配置清单或设备编译计划，仍保留源码树范围并给reason。成功编译记录用于证明这些源码参与，不能反过来只保留已成功记录的文件以消除覆盖缺口。

`compilation_evidence.receipt_count`是收集到的执行记录数，不等于有效编译单元数或成功目标数。`records`仍须满足实际命令、工作目录、有效对象及依赖等条件；临时预处理输出不作为对象编译证明。执行记录可来自私有CMake launcher、Python子进程观测或可用的strace。默认保留`bisheng`/`ccec`的PATH发现路径；其PATH转发仅由`ACD_OBSERVE_COMPILER_PATH=1`显式启用，因为转发会改变依赖编译器位置的SDK发现。硬编码入口、自定义launcher或覆盖观测环境仍可能留下缺口，不能由receipt数量推断所有SDK路径均已覆盖。机制及边界见[脚本契约](using-scripts.md)。

SDK生成包装源码、删除临时对象再链接或打包最终产物时，成功执行记录也可能无法建立源码到最终产物的完整链，因而`records`不足、覆盖仍为unconfirmed。保留工具状态，结合完整原配方日志、目标/架构及最终产物另述实际构建结论；不要把证据缺口解释为修复失败，也不要仅为改变标签重复构建。

`audit` 单列本地审计持久化：`status=written` 附实际 `path`；失败为 `status=failed`、固定 `reason` 及可选异常类名 `error_type`，不改变编译结论。 written 只证明写入；脱敏不可用时仅保存明确占位并告警，不回退原始内容。 path 是本地未脱敏路径，外发前仍须脱敏。目录选择与只读安装约束见 [using-scripts.md](using-scripts.md)。

新增状态 `compile_failure_remaining` 表示仍失败但无适用前态，`verification_interrupted` 表示启动后超时或中断；before/after 可为 `interrupted`/`not_started`。`--previous-result` 接受完整 verify.json 或 diagnose 的 failure_evidence.json。可选结构：`failure_evidence` 保存当前源码、构建配方和问题身份，`target` 表示 SoC，另以 `build_target` 和可选 `selected_example` 绑定本次构建目标；`error_comparison` 保存 remaining/disappeared/observed_new 或不可比原因；`compilation_evidence` 与 `target_validation` 保存执行、对象、依赖及目标证据；`validation_scope` 区分 compile/contract/numerical。contract 轴来自顶层 `contract_check`（带前态时自动运行）：报错至少两处的失败标识符在修复后于源码中零出现即 `violated`，transition 降级为 `compile_passed`，reason 为 `functionality_identifiers_removed`，missing 按标识符含 `prior_error_sites`；单点消失记 `tolerated`，不判定为违约。可选顶层 `escape_suspect` 只告知、不阻断，仅在发现疑点时出现；缺席不等于已检查且干净。CLI 写出完整 JSON，下一次复用该文件，不要把摘要当作完整前态。请求 SoC 省略时 `soc_requested=null`，派生目标与真实编译目标分开读取。

`error_comparison.build_target_comparison`保存before/after的build_target与selected_example， status为matched、conflict、unknown或not_recorded。只有matched支持compile_failure_fixed；缺失或冲突时可以比较诊断、本次编译成功仍为compile_passed，不推断旧目标已修复。完整旧verify结果可保留其已有expected_target，纯日志不从当前环境补造历史目标。旧失败文件的依赖覆盖仅使用本次匹配target/arch的记录；同header、不同目标不互相代验。

`error_comparison.recipe_changed`为true/false/null；分看`recipe_files_changed`与 `recipe_arguments_changed`。前者沿用`recipe_sha256`的配方文件hash，后者比较完整verify证据中的`recipe_request_sha256`（soc_used/build_args/build_target/cmake_args，保留参数边界）。任一变化为true；两项均已知不变才为false；其余为null。单纯日志诊断没有请求参数前态，不能以配方文件未变推断argv不变；这些字段也不证明完整编译器调用或环境相同。

`target_validation.prior_failure_coverage`是对象，其`status`为`matched`、`unconfirmed`或`not_applicable`，另有`reason`及可选`required_files`/`missing_files`/`unresolved_files`。摘要在顶层保留`prior_failure_coverage`对象。报告保留实际字段路径、原值及来源，不将它改名为泛指的`coverage`；输入若用`prior_failure_coverage=unconfirmed`简写，则原样引用并说明“旧失败覆盖未确认”，不将简写误写成JSON对象的实际类型。

按完整工程归属路径核旧失败文件进入本次依赖闭包，不能用别的工程同名文件抵充。`status=unconfirmed`时`compile_passed`不证明旧失败已修复；仅无历史时可结合已有上下文判断，不为凑fixed标签追加调用。完整编译证据另含`project_root`。

`primary_error`摘要及完整`failure_evidence.issues`保留`file_original`、`file_resolved`和 `file_reanchored`。resolved仅表示当前工程内可读；reanchored表示按目录或编译器源码回显推断的阅读候选，源码回显候选另有可选`file_reanchor_method="compiler_source_echo"`和 `file_reanchor_source_sha256`（本次读取的当前源码字节hash）。只接受可靠错误块的完整回显、同文件名同物理行及完整有界扫描中的唯一匹配；上下文读取内容变化时撤销候选，不重试。无法建立物理行映射的续行拼接、原始字符串或`#line`等情形保持未解析。 hash和同一行回显均不证明历史文件身份，也不证明周围声明、Host/Kernel作用域或构建条件未变。此类旧文件列入`prior_failure_coverage.reanchored_files`，即使当前候选进入依赖闭包也保持unconfirmed。原始verify日志只绑定当前直接路径，不暗中推断历史后缀。同一问题去重时保留推断来源，另一条当前直接路径诊断不能抹去它的历史不确定性。诊断产物内`context.json`的`source_coverage.failed_source_reanchored`和 `source_coverage.excluded_external_sources`分别记录候选来源及未读取的工程外源码链接；这两项不在stdout摘要中，按需读取context，避免把当前源码误作旧日志源码。

### 标识符消失后的契约判读

`contract_check=violated`是标识符消失的风险提示，不能直接认定删除功能。结合最终diff，先区分两种形态：保持契约的统一改名，或删除调用/功能。

- 统一改名：记录映射，并核对当前SDK声明、参数/类型、调用及数据路径、用户指定的实现约束和原目标编译证据。映射本身不证明等价；旧声明是否仍存在也不能单独决定修法。
- 删除调用/功能：恢复必要使用点和语义，再作保留契约的修复，沿原配方重验；不得用删调用、stub或换目标来消除报错。

若仍需修改、最终源码未覆盖或存在行为风险，补齐相应验证；已经具有最终源码、原目标及契约的完整有效证据时，可直接引用，不为改变状态标签重复构建。保留原始`violated`，另述人工核验依据和结论。单点消失的`tolerated`以及`status=ok/not_applicable`仅说明该启发式未判违，不证明功能等价，也不取消原任务的必要验证。
