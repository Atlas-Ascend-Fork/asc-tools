# 脚本使用契约

diagnose、inspect、verify及辅助redact调用随包确定性引擎。修复由调用方完成，工具不生成补丁、不调LLM、不运行修复循环。操作次序见[SKILL.md](../SKILL.md)；本页维护真实参数、返回和故障处置。

## v2默认入口与兼容

CLI的`diagnose`默认`--context-mode light`：只解析日志，返回`schema_version=2.0`、`observation_status`和首错；不读取工程源码、不探测环境、不构建。`unknown`表示没有足够失败信号，不表示成功。`focused`启用围绕首错的有限取证和现有诊断规则；`full`保留旧完整包路径。Python内部`run_bundle`默认full供既有调用者兼容，调用方可显式指定模式。旧报告的passed不升级为新协议的编译确认。

```bash
python3 "$SKILL_DIR/scripts/run.py" inspect --project "$PROJECT_DIR" \
  --file src/kernel.cpp --max-files 16 --max-bytes 262144 --out "$REPORT_DIR/source.json"
python3 "$SKILL_DIR/scripts/run.py" inspect --project "$PROJECT_DIR" \
  --question build --build-dir out/debug --out "$REPORT_DIR/build-facts.json"
python3 "$SKILL_DIR/scripts/run.py" inspect --project "$PROJECT_DIR" \
  --question environment --out "$REPORT_DIR/environment.json"
python3 "$SKILL_DIR/scripts/run.py" inspect --project "$PROJECT_DIR" \
  --question knowledge --mechanism header_origin --out "$REPORT_DIR/mechanism.json"
python3 "$SKILL_DIR/scripts/run.py" inspect --project "$PROJECT_DIR" \
  --question knowledge --symbol AscendC::Exp --cann-version 9.0.0 \
  --limit 5 --out "$REPORT_DIR/api.json"
python3 "$SKILL_DIR/scripts/run.py" verify --observe-run "$RUN_DIR"
python3 "$SKILL_DIR/scripts/run.py" verify --recover-verification "$RECOVERY_DIR" \
  --json-out "$REPORT_DIR/recovered.json"
```

inspect的`--out`是完整JSON；stdout给状态、位置、证据绑定/时效/缺口及读取计量，不展开源码和文件hash。操作完成与证据充分分别见`operation_status`和`evidence`。`partial`是已有可用片段但仍有缺口，CLI退出0；只有查询错误退出1。`completed`不等于构建、保持或修复验收，`acceptance`固定为`not_assessed`。

source可用`--build-log`提供失败文件，或用`--file`显式指定；优先失败单元及字面include闭包，再读构建脚本。`--max-files/--max-bytes`默认16/262144；`--max-metadata-files/--max-metadata-bytes`默认128/4194304，单独覆盖数据库、Make、File API、CMakeCache和根CMake读取。预算限制文件内容读取，不限制目录枚举耗时或整个JSON大小。完整结果分别给`read_evidence`与`metadata_read_evidence`；文件hash证明本次内容身份，不证明配置未过期。

build只读已有记录，未配置不自动configure；source/build均可用`--target`（CMake目标）与`--configuration-id`筛选。无记录、请求未命中、多配置未消歧和相关命令未展开均保留缺口。显式`--build-dir /工程外/实际构建目录`只把该目录加入读取范围，不包含其父目录或其他工程；默认build符号链接指向项目外时也须显式选择。source可沿选中命令的include目录读取此范围内的生成头，或用绝对`--file`查询其中的生成源码。

常见GNU驱动的`@response`按受限语法展开：嵌套相对路径以命令cwd为基准，保留原参数、展开参数、响应文件hash和缺口；不做shell变量/命令替换。循环、越界、未知驱动/语法、编码或预算问题保留partial。响应文件与其他构建元数据共享默认128文件/4MiB读取预算；展开另限16层、每条4096个token/4MiB处理文本、整次查询32768个token/8MiB，缓存文件每次展开也计入。原始argv不被展开限额截断。预算不足时沿选中命令单独核对未决文件，不擅自改写原构建记录或把未展开内容当空参数。

不要把`replayable`理解成已执行或当前失败的配方证明。响应文件变化会使configuration_id变化，旧选择器需重新查询；invocation_id仅绑定原始argv/cwd/output。source返回词法依赖/定义候选，选target不会自动执行预处理。环境查询不检查设备。

knowledge不传symbol时查询机制卡；`--mechanism`与`--symbol`互斥。API查询可补`--namespace`、`--api-level`、`--cann-version`、`--cann-build`、`--target-arch`及`--limit`（1—50，默认5）。架构参数是实际编译命令/SDK证据里的架构码，不是从910/950家族猜出的值；未知字段省略。先按现有诊断过滤器筛选，再截取显示，返回候选总量/被筛除数/省略数与来源、适用性缺口。版本未知不冒充9.0；超出已核实版本域保留提示。查询不读工程、不探环境、不构建；`no_match`仅表示本地知识未找到兼容候选，不能证明API不存在。按输出缺口核当前SDK/调用域，必要时定向使用已安装的CANNBOT基础技能；不为单个符号默认跑full诊断。

`--run-probes`或`--verify`须显式选择focused/full；light不隐藏执行。独立verify可加`--recovery-dir`指定新的持久记录目录；`--observe-run`只观察执行，`--recover-verification`收取已有阶段并重算验收，不派发新构建。原副本、证据和环境不再匹配时保留未完成，详见[执行状态](repair-state.md)。收据passed只确认命令与日志；完整verify报告另外判断覆盖、前后问题及保持检查。

verify报告阶段异常时退出码为1；已发生的构建保留`build_invoked=true`，不能确定是否启动为`null`，只有确定未启动才是`false`。报告写入失败会在stderr给出`report_write_failed`和恢复位置。此时先观察原运行，不因CLI失败重新派发。`--recover-verification=DIR`与空格写法均可用。

复杂任务另有可选`run.py memory --project DIR [--state JSON] [--event JSON] --out NEW_JSON`，保存显式证据依赖及宿主决定，不编译、不调用模型。机制查询可带`--memory JSON`和必要的`--allow-root PATH`；仅复核已记录文件，并返回下一待决问题。格式与成本边界见[证据状态](evidence-memory.md)，不要将其作为简单题强制前置。

## 支持环境与真实入口

Linux Python 3.9+与PyYAML；真实编译另需 CANN ≥9.0.0、对应工程依赖和可用构建工具链。具体版本域以随包登记和当前安装声明为准。`SKILL_DIR`取自本次实际加载的SKILL目录，不依赖开发仓库或固定全局位置。`.sh` 包装器在非 Linux 环境（含 Windows/PowerShell/Git Bash）返回支持范围提示，不提供 Windows 编译或诊断支持。

以下分别展示三个独立入口的位置参数形式，按当前证据缺口选用；不是要求依次全部执行。额外flags传给底层CLI：

```bash
mkdir -p "$REPORT_DIR"
bash "$SKILL_DIR/scripts/diagnose_compile_failure.sh" \
  "$PROJECT_DIR" "$BUILD_LOG" "$REPORT_DIR/diagnosis" "$SOC" --json-stdout
bash "$SKILL_DIR/scripts/run_verify.sh" \
  "$PROJECT_DIR" "$REPORT_DIR/verify.json" --soc "$SOC"
bash "$SKILL_DIR/scripts/redact_bundle.sh" \
  "$INPUT_FILE" "$OUTPUT_FILE" --stats "$STATS_FILE"
```

也可使用Python完整flags形式：

```bash
python3 "$SKILL_DIR/scripts/run.py" diagnose \
  --project "$PROJECT_DIR" --build-log "$BUILD_LOG" --out "$REPORT_DIR/diagnosis" \
  --soc "$SOC" --json-stdout
python3 "$SKILL_DIR/scripts/run.py" verify \
  --case "$PROJECT_DIR" --json-out "$REPORT_DIR/verify.json" --soc "$SOC"
python3 "$SKILL_DIR/scripts/run.py" redact --in "$INPUT_FILE" --out "$OUTPUT_FILE"
```

解释器不在PATH时，shell入口用 `ASCENDC_DOCTOR_PYTHON` 指定实际Linux Python路径；直接Python入口显式运行该解释器。完整参数见 `python3 "$SKILL_DIR/scripts/run.py" diagnose --help`（verify/redact同理）。verify可选 `--previous-result PATH`，接受前次完整 verify.json 或诊断产物 failure_evidence.json。

安装、解释器或环境能力存在疑问时才运行预检；已有有效检查直接复用，不在每次诊断、验证前重复预检。

verify 也接受 `"$PROJECT_DIR" --json-out "$REPORT_DIR/verify.json"`，包括 `--json-out=PATH`。工程和输出各指定一次；位置参数与同名flag重复、或重复的 `--case`/`--json-out` 会在构建前明确报错，不能靠后一个值覆盖前一个。只有工程位置参数、缺输出时，会给出完整用法。

引擎会创建自己的报告目录及JSON父目录。**shell 重定向发生在脚本启动前**：若使用 `> "$REPORT_DIR/diagnosis-summary.json"`，仍须先 `mkdir -p "$REPORT_DIR"`，否则脚本尚未执行就会失败。为每轮使用独立报告路径，保留前次证据。

### 保留原配方与全部目标

先从任务和原构建入口取得完整命令、工作目录、参数与目标，再选择调用方式：

| 原配方 | 调用方式 |
|---|---|
| 工程已有 build.sh | 保留脚本的全部阶段；原参数用重复 `--build-arg=ARG` 传入 |
| 无 build.sh，指定单个 CMake target | 传一次 `--target NAME`，原配置参数逐项用 `--cmake-arg=ARG` 传入 |
| 原任务要求多个目标 | 使用覆盖全部目标的原脚本，或逐目标单独验证、分别保存结果并汇总；同一源码的不同宏或类型实例也分别核验 |
| 原配方不能由当前入口完整表达 | 直接沿原命令真实构建并保留日志、退出码、目标/源码/架构及产物证据；不删必要参数迁就工具 |

`--target` 是单目标参数，重复或空值在构建前作为参数错误拒绝；这时没有本次 verify JSON，读取stderr修正调用，不能复用旧文件冒充本次结果。省略target可能构建默认目标，不能认为自动继承任务中的目标。构建脚本中的多目标调用仍合法，参数限制不改变用户的全部目标要求。

例如原配方指定 `-DCMAKE_BUILD_TYPE=Debug` 和目标 `vector_add`，应传 `--target vector_add --cmake-arg=-DCMAKE_BUILD_TYPE=Debug`。缓存赋值须带`-D`；裸`NAME=VALUE`返回`verification_not_run`，reason为 `cmake_cache_argument_missing_define`，build_invoked=false且before/after为not_started。修正调用后再验证，不把调用问题算作新源码错误。固定框架adapter对不支持的额外参数明确not_started。

包内 Python/shell 入口也接受 `--cmake-arg '-DOPTION=VALUE'`：每个 `-D` 值保持为一个 shell 参数，转发时不拆分、不求值。包含空格或分号的值保持完整，例如 `--cmake-arg='-DLABEL=hello world'`。多个 CMake 参数分别重复单数 `--cmake-arg`；复数 `--cmake-args` 会在调用引擎前明确报错并给出用法，不猜测如何拆分一段命令字符串。

`--soc`需准确SKU，例如`Ascend910B1`/`Ascend950`；架构号`2201`/`3510`会在构建前返回verification_not_run和reason=soc_requires_sku。SoC不传时soc_requested=null，由manifest/project/default派生，仍须核实际目标证据。完整CATLASS根没有前两者、但已明确example与框架环境arch=3510时，可从该配置推导Ascend950，soc_source=framework_env；无关工程不继承该框架环境目标，显式调用方仍优先。

安装目录只读。包装器在位置参数规范化后，检查 `--out`、`--json-out`、 `--stats`、`--meta-out` 的真实输出目标，包括等号形式和CLI接受的缩写；任何一个指向安装目录都会在写主输出前拒绝。按本技能流程将报告放到工程外的独立目录；构建/探针临时目录也须在源码树外独占，不能将TMPDIR指向待复制源码。

审计默认写入本次授权输出旁的 `audit/audit_YYYYMMDD.jsonl`（verify/redact 在主输出文件的父目录内；diagnose 当前不产生审计事件，redact 可逆模式同样不落账）。若该位置仍在输入源码树内，则使用独立用户状态目录（Linux 为 `${XDG_STATE_HOME:-$HOME/.local/state}/ascendc-compile-doctor/audit`）。可用 `ACD_AUDIT_DIR` 显式选择外部目录；安装树或输入源码树内的位置（包括符号链接）会在执行前拒绝，不会静默换路径。verify 的 `audit.status=written` 及 `audit.path` 只证明审计已写入，不是编译成功；失败返回 `status=failed`、固定 `reason` 和 `error_type`，stderr 同样只报固定码与类型，保留原编译结论。脱敏不可用时字段留占位并告警，不回退原值。也可用 `PYTHONPATH="$SKILL_DIR/scripts/engine" python -m harness.engine.audit tail -n 10 --audit-dir PATH` 读取明确目录，包括已有历史审计记录。`audit.path` 是本地实际路径，分享 verify 结果仍须脱敏。

### CATLASS构建根与覆盖源

若已有完整CATLASS树（scripts/build.sh及examples），将它的工程根同时用于diagnose与verify，按已知任务配置`ASCENDC_CATLASS_ROOT`、`ASCENDC_CATLASS_EXAMPLE`与准确SoC。整树验证在独占的每阶段副本中执行，保留目标例的改动，只取该例的编译覆盖；框架根 CMakeLists与其他examples不能作为补丁覆盖进目标例。失败证据保留`examples/...`相对身份。

独立例的源文件也可作为overlay输入，配完整外部框架。若overlay源目录又含完整框架子树，工具拒绝含混根目录；先确定既有的唯一待修树。不要为凑fixed标签新建两文件case或按同名文件强行关联旧证据，换工程或确实缺前态时只保留本次compile_passed。

当前固定CATLASS适配器通过`scripts/build.sh`构建已配置的目标例和arch；重复build_args/build_target/cmake_args仍会明确拒绝。只有确认额外参数完全重复已配置的目标/arch，才省略重复项，使用例如`--soc Ascend950`重试；若原任务确有不同构建选项，保留这些选项，沿原配方独立构建并记录证据，不能删掉必要参数换取通过。自包含`operators/<op>`工程及其平级依赖不能自动当作example overlay，须按其真实CMake 依赖与配方确认可复现性；本段不宣称已验证所有官方生成工程布局。

## 诊断产物与状态复用

诊断准入以[SKILL.md第一动作](../SKILL.md#第一动作解析已有日志)为准：足证快路径可直接修改并验证；其余已有完整失败日志的情况，第一步先运行或复用轻量诊断，不将其列为长期搜索后的可选项。首错摘要之后才补声明、条件头和生成/消费链证据，确需联合规则时再显式用focused诊断；不以重复解析同一日志代替取证。

首诊参数与light/focused/full的副作用统一见[v2默认入口与兼容](#v2默认入口与兼容)。未执行构建、未请求源码或环境取证都不表示编译成功或相应能力缺失。

同日志内容与解析版本的结果可复用。新日志先解析；若仅行号移动且已核同一声明/调用及配置，复用仍有效的根因解释。源码、目标、宏/include或SDK变化使受影响解释失效。`unknown`或建议被反证本身不触发相同输入重跑；继续核声明与必要构建，不能拿诊断代替验收。

诊断命令未实际执行、执行报错或摘要缺失时，分别记录实际状态。可修正一次明确的路径/参数问题；解释器、依赖或宿主权限仍阻断时，说明缺失能力并沿原日志继续。工具故障不算源码修复失败，也不授权安装依赖、扩大访问或绕过保护。

大日志传文件路径，保留完整首错和模板上下文。`--json-stdout`给出摘要，先完整解析JSON再选字段展示；不要用tail截断。更多证据按返回的 files/bundle 字段定位，截断、缺失、未覆盖不能当作“已排除”。

| 字段 | 判读 |
|---|---|
| `error` | 诊断自身失败；空category不能当未知编译问题 |
| `category` / `phase` | 检索和阶段线索，可能由消息、类别或路径推断；混合翻译单元按报错处函数与实际编译上下文核对，不凭后缀决定Host/Device修法 |
| `failure_stage` | 直接日志阶段及basis；log_line是归一/重排后日志行号，不是原始物理行；unknown保留原因 |
| `source_context` | 当前可直接绑定的简单函数线索，值host/device/unknown；不证明历史源码、宏展开或实际执行域 |
| `diagnostic_facts` | diagnostic-facts-v1；mechanism按所选primary文字辨认语言/工具机制，sdk_association分别报告未建立/明确根namespace/原SDK声明note关联/不适用；不从旧category或名字相似反推SDK归属 |
| `path_hint` / `conflicts` | 路径仅是提示；冲突与旧phase分开列出，按原诊断和源码消歧 |
| `version_compatibility` / `environment_checks` | 分开核工具链来源、版本、环境能力；mismatch降级版本敏感建议，unknown保持未知 |
| `primary_error` | 当前首错；`file_resolved=false`时先定位真实文件；`file_reanchored=true`仅为旧路径推断的当前阅读候选，原路径见`file_original` |
| `root_cause_localization` | 跨文件根因候选及证据，需与当前定义/使用方核对 |
| `fast_path` / `self_explanatory` | 建议的短路径与定位线索，不是免核前提或禁止取证 |
| `kb_evidence` | 签名/声明头/版本/arch的参考证据；检查来源及适用性 |
| `top_suggestion` / `next_actions` | 条件修复指导；diagnostic_guidance项的patch恒空，读action/mechanism/applicability/compiler_notes，不作为已生成补丁或正确修法 |
| `verify` | 诊断期best-effort编译探针摘要，不代替修复后的验证 |
| `files` / `bundle` / `run_id` | 完整产物与追溯入口 |

### 轮次证据闭环

把编译修复当作可追溯的轮次链，而不是脚本内部的自动修复循环：

1. 原样保留最初失败日志和配方；已诊断时同时保留 `failure_evidence.json`，不要用后续结果覆盖。修复后才发现没有机器前态时，不为取得 `compile_failure_fixed` 撤回有效修改、重新制造失败；沿原配方验证当前源码，报告本次结果及前态证据的限制。
2. 每轮只记录一组连贯源码或构建配方改动，单独保存该轮的 `verify.json`、日志、改动说明和结论。报告至少写清改了什么、实际编译覆盖了什么、首错是保留/消失/变成新错，以及哪些风险仍未核验。
3. `run_verify`只验证调用方已经做出的改动，不会自行修改源码、调用其他skill或循环修复。出现新首错或反证时，解析新日志并更新问题。`--previous-result`优先使用完整verify报告；light没有源码快照，focused的日志证据也不能冒充完整失败前态。只有包含有效源码/配方身份的证据才可用于前后覆盖比较。直接构建的原始日志和配方同样保留，不虚构机器前态。
4. 最终编译通过时审核整条轮次链：最初失败是否在同一目标和必要编译单元中得到覆盖、各轮是否有真实构建证据、接口与功能契约是否保持。`compile_failure_fixed` 只表示编译层前态对比成立；`compile_passed` 只表示本次编译通过或前态不可比。两者都不能替代运行或数值验收。
5. 同一首错经过至少两轮真实验证仍失败，且没有能区分假设的新证据时，停止重复调用；先写交接报告，再按停滞类型调用一个适用的已加载 skill。无证据时不遍历全部路由，也不把“建议换技能”丢给用户作为收尾。

公开诊断的 `patch_suggestions.json` 保留旧文件名，格式为 `schema_version=compile-repair-guidance-v1` 与 `suggestions` 数组。普通项为 `type=diagnostic_guidance`、`risk=unknown`、`patch=""`、`applicability=conditional`， `action` 按已观察机制说明需比较的证据与修复条件，不因旧category指定dtype、强制转换或删除参数。新指导只消费已传入的诊断/归一化证据，不再为拼补丁额外扫描全工程；上层调用方决定并实施修复。

`compiler_notes` 最多三条，保留归一化日志行号、原文件位置和同块绑定。 `diagnostic` 最多360字符，`source_excerpt`最多240字符；它们是预览，不保证完整声明。同块摘录不能替代 `diagnostic_facts.sdk_association` 的独立来源要求，也不证明版本/目标适用。没有首错或 `no_compile_error` 时 suggestions为空；历史门控建议卡和历史建议type仍可读取。

分别解释编译阶段、语言失败和SDK归属。普通未声明对象或工程成员先核当前函数/类的声明与使用，不因变量名含tiling就补schema；SDK归属未知时，仍可沿具体名字查找失败或重载拒绝定位。 SDK/API判断须有当前调用与关联声明等依据，不能把类别名当作某API被移除或某目标不支持的证明。完整diagnosis中的根因候选保留与首错相关的项目检查；已有明确编译器/工具错误时，诊断机器缺SDK等就绪检查不会占据根因候选。compile_commands生成提示也不作为失败原因。全部静态检查仍在诊断产物中保留，可按需查看；离线分析能力与重新构建能力分别判断。源码中的CCE字样、注释、无关声明和手写入口名错配不触发compiler_internal升类；名称不一致先核报错调用与声明。实际编译器崩溃诊断、探针解释仍独立取证，撤回字串推断不等于已经证明未知错误的根因。 KB可用首错回显中的无歧义限定调用，在展示候选前筛选namespace；这不推断实参类型、重载选择或当前目标适用性。复杂表达式保持未知，不能把候选列表当作完整SDK重载集合。

版本或目标不一致时，先按已确定的用户意图检查实际 toolkit、工程构建配方和 target；可以在授权范围修环境。只有选择仍不明确、会改变接口或功能时才确认意图，不把建议字段变成每次都向用户索要许可的规则。

### fast_path 可被证伪

| kind | 动作 |
|---|---|
| `compiler_suggested_rename` | did-you-mean是候选。若声明、实参和契约支持局部替换，直接改并验证 |
| `kb_known_symbol` | 先用附带signature/header；参数、namespace或目标不符时核当前声明 |
| `signature_deterministic_fix` | 核action前提与保留功能；删除报错点不等于修复 |
| `needs_localization` | 先对照相关定义/使用与任务要求，仍缺外部契约时再查；后缀本身不证明谁被改坏 |
| `ambiguous_fix` | 多条规则候选同命中，附候选集；按声明、目标与调用契约区分后择一 |
| null/未知 | 从当前首错与相关源码定位，按需读类别页或取最小探针 |

`kb_near_name`只表相似；工程自有符号不能因此当vendor API改名。聚合头没导出或短名没命中也不能证明API本体不支持。建议与源码、目标声明或真实编译冲突时，撤销该 fast_path 假设及自己的无依据试探，说明反证，保留用户和已证实的改动。

定位输出是候选，本地定义、消费方和任务约束完成核对后即可修补。`unknown`、低置信度或 `decision_advisory` 不要求继续检索已经消除的歧义；确有声明或语义缺口时才查对应 SDK 资料或调用适用的 CANNBOT 技能，协作名单见[SKILL.md](../SKILL.md)。

## 验证方式与结果

默认只给工程目录和JSON输出，在独占副本验证**当前工作副本**，返回 `verify_mode="in_place"`；无需先序列化patch，没有 `write_back`。这只描述入口和工作副本，不表示默认构建参数与用户原配方相同；原target及CMake/build.sh参数须显式传入。普通CMake入口不隐式添加Debug；未指定build type时沿工程默认，原显式build type参数原样传入。能够完整表达原配方时，一次run_verify即可完成编译取证。已有充分本次证据时直接复用，额外重编应针对明确的证据缺口；不能为了收集结构化字段重复已确认的构建。独立用户补丁可额外传 `--apply-patch "$PATCH_FILE"`；历史或用户提供的JSON确实含真实diff时，也可用 `--patch-suggestions "$JSON_FILE"`。当前诊断的guidance中patch恒空，不能作为可应用补丁。验证已编辑的工作副本通常省略这两项；有独立diff时使用patch模式。 `--dry-run`只验证流程/补丁，不是真实编译通过。

读取JSON的`transition`、before/after、build_invoked、日志和证据，而非只看CLI退出码。`compile_passed`与`compile_failure_fixed`是`transition`的取值，不是顶层布尔字段；读取不存在的同名键得到null不表示构建失败。顶层`built_target`/`built_arch`是实际观测目标/架构，须与原配方逐项对应；日志和工作目录用`after.log_path`/`logs.after`及`after.recipe_cwd`定位。无适用前态时只报本次结果及“变化未知”，不能凭synthetic before判断同错/新错。既有全部transition取值保留兼容；未知新增值按“未确认验证”读取其reason与证据：

包内 `verify` 入口的 stdout 保持输出结果 JSON 路径；stderr 输出 `[verify]` 单行 JSON 摘要，`schema_version="verify-summary-v1"`。原有 `result_path`、`transition`、`build_returncode`、`compilation_evidence`（状态字符串）、`log_path` 和 `recipe_cwd` 六键保持兼容，另提供实际目标/架构、覆盖、契约、数值范围和异常原因，字段见[JSON契约](json-schemas.md#验证摘要)。摘要不展开编译记录、依赖哈希或完整失败证据。

字段缺失保持 `null`；摘要不由返回码、产物数量或缺失字段推断通过。构建返回0而覆盖 `unconfirmed`、编译通过而契约 `violated`、数值 `not_checked` 都会照实保留。完整日志可能名为 `verify_before.log`；直接打开返回的 `log_path`，不要猜 `verify_after.log`。摘要读取失败只提示查看完整结果，不改写原JSON或入口退出状态。

### 结果短读与产物复用

先读取现有 `[verify]` 摘要。要重新查看已保存报告时，使用只读入口；它把同一摘要写到 stdout，不导入引擎、不调用构建、不写审计或修改报告，也不重新判定当前源码是否仍与报告一致：

```bash
python3 "$SKILL_DIR/scripts/run.py" verify \
  --summary "$REPORT_DIR/verify-r1/result.json"
```

`--summary=PATH` 也支持，不能与构建参数组合；没有verify的 `--json-stdout` 参数。只读摘要不依赖CANN或PyYAML，可离线查看已有JSON。读取成功返回0，只表示读取成功；文件缺失、JSON损坏或所读字段类型不符返回2；参数形态错误返回64。旧报告缺失字段保持未知，未知的transition原样显示，不升级为通过。摘要不作为 `--previous-result`；前态始终传原完整JSON。

先看 `validation_scope` 的三个轴、`target_validation`、`prior_failure_coverage`、`contract_check` 及 `escape_suspect`。后者存在时，即使 `compile_failure_fixed` 与契约 `ok`，仍需核对原报错符号为何消失；摘要保留检查名、缺失数量和实际证据日志，不展开长建议或符号列表。缺席为null，不证明检查已完成或无疑点。只有摘要仍留有疑问时，再读对应的 `missing_files`、`unresolved_files`、`escape_suspect.missing_symbols`、编译记录或契约细项；不要为找字段重新构建，也不必 `cat` 整份回执后反复 `head`/`tail`。覆盖数量只用于定位证据缺口，不代替目标、架构与源码参与的判断。

沿 `after.recipe_cwd` 和原构建配方确定产物位置，核对应目标、非空大小与当前构建关联；不猜固定临时路径。若副本的产物、依赖和运行所需资源完整且仍可用，后续检查可直接在该副本进行。任务需要原位置安装包、原工程产物、额外构建阶段，或副本不能满足必要运行条件时，再完成对应步骤；这些目的不能用一次副本编译替代。没有新增源码或验收缺口时，不再原位重编同一目标。

### 验证状态

| transition | 判读与处置 |
|---|---|
| `compile_failure_fixed` | 编译层通过；仍核实际 target/编译单元/依赖参与、escape_suspect及任务契约；带前态时含标识符保留契约检查，violated已降级为`compile_passed`（见下文contract_check） |
| `compile_passed` | 当前编译已确认；工具没有足够证据判定旧失败变化，缺机器前态与目标/覆盖冲突须分别处置（见下文）；契约violated的降级也落在此状态（reason=functionality_identifiers_removed，读contract_check） |
| `same_error_remaining` | 仍有错误；不自动证明具体问题相同，不机械重套同一建议 |
| `new_compile_error` | 仍有错误；先据完整日志分清是否出现新问题，文本或位置改变不自动证明进展或要求重复诊断 |
| `compile_failure_remaining` | 本次编译仍失败，但没有适用前态，变化未知 |
| `verification_interrupted` | 构建已启动后超时/中断，验证未确认 |
| `patch_apply_failed` | 独立patch未干净应用，查上下文/层级，或直接验证当前工作副本 |
| `no_source_found` | 工程目录/布局未找到源码，检查CASE_DIR，不是修复成功 |
| `case_unsafe` | 无法安全隔离，按reason修正输入或取可隔离工程 |
| `verification_not_run` | 没进行真实编译，不能判定修复成败 |
| `environment_unavailable` | 环境或构建入口不可用，先修工具/构建配方，不据此改业务源码 |
| `dry_run_apply_ok` / `dry_run_env_override_ok` | 干跑通过，不等于编译通过 |
| `case_not_reproducible` | 本次证据不足以确认所需编译覆盖，按reason核目标、源码、架构及日志；不能解释为旧错误修好后不再复现 |

返回未知transition、缺transition、超时、缺日志或没有构建覆盖证据时，保持“未确认验证”，读取stderr、退出码、reason与日志，不虚构修复成败。环境修复依用户已授权范围继续；没有新证据时停止重复同一调用。

覆盖未确认时，先区分“未编译”和“编译记录不足”：现有日志若已含最终源码的实际编译、原目标及架构和所需产物，引用这些证据并保留工具原状态；若确有目标未执行、最终源码未参与或命令缺失，再按原配方补验。直接报告人工核验的范围与仍未知的部分，不为把状态改成成功标签重编相同内容。任务要求全新构建时，清理路径按构建命令的真实cwd解析，核实实际build目录确实被重建。

读取可选 `failure_evidence`（源码/构建配方/问题身份）、`error_comparison`（错误差异或不可比原因）、`compilation_evidence`、`target_validation`和`validation_scope`。`contract_check.status=violated`及`reason=functionality_identifiers_removed`表示标识符消失的契约风险，本次编译事实仍可为`compile_passed`；词法阈值、tolerated和人工复核统一见[JSON契约](json-schemas.md#标识符消失后的契约判读)，本页不另设判据。before/after可含interrupted/not_started，in_place不构造历史前态；`build_invoked`依据实际启动。CLI完整结果写入JSON_OUT。

验证在本次构建子环境中使用私有CMake转发与编译launcher、Python子进程观测收集执行记录，默认保留`bisheng`/`ccec`的PATH发现路径。不改SDK文件或全局环境，保留原compiler/flags/target及用户hook；CMake入口仍由私有转发包装。strace仅为可选预检fallback，禁ptrace不阻止原构建。硬编码绝对入口、自定义launcher、Python `-S`/`-I`或覆盖观测环境可能留下缺口；这些机制不保证覆盖所有SDK构建路径。

仅在已确认工程不依赖编译器安装位置推导SDK时，调用者可显式设置`ACD_OBSERVE_COMPILER_PATH=1`，启用`bisheng`/`ccec`的私有PATH转发以观测原生make子进程。此选项会改变`command -v`等路径查询结果，即使编译参数不变也可能影响构建；不要为获得确认标签默认启用，也不要修改用户工程来迎合观测入口。

执行记录还须匹配实际工作目录、命令、目标、依赖及当前有效对象。临时预处理输出、已删除对象、touch旧对象或打印命令均不能补足编译证据；观测不足时保持unconfirmed，按已有构建日志与原配方补齐证据，不为获得成功标签改编译器参数。

部分SDK配方从生成的包装源码编译临时对象，再删除中间文件并链接或打包成另一最终对象。即使已记录编译器成功退出及临时对象hash，缺少“原源码依赖→生成源码→临时对象→最终产物”的完整关联时，工具仍可返回unconfirmed。这表示证据链未闭合，不等于实际构建或修复失败；先核对原配方完整日志、全部目标和架构及最终产物，另述已确认事实与工具缺口，不为改变状态标签反复重编。

`target_validation.prior_failure_coverage`区分matched/unconfirmed/not_applicable；按完整工程归属路径查旧失败文件是否进入本次真实依赖闭包。单凭unconfirmed或本次compile_passed不能声称旧失败已修复。如果只是机器前态不可消费或执行记录不足，而原目标、旧失败相关源码在最终构建中的覆盖及功能契约已有完整证据，可另述基于日志与源码的人工判断，保留工具原状态与未确认项，不为fixed标签追加调用；这不能补偿真实目标冲突或源码未覆盖。

旧日志路径即使仍可读取，也不能替代当前工程源码。唯一目录后缀匹配可以提供阅读候选；工程搬动或展平后，也可按同文件名、报告行号及编译器源码回显的严格唯一匹配提供候选，但其`file_reanchored`不证明历史身份；`reanchored_files`保留此类旧文件，覆盖仍为unconfirmed。相同行可被复制，周边声明和Host/Kernel作用域仍可能不同，须阅读当前上下文后再判断修法。回显候选用可选`file_reanchor_method=compiler_source_echo`及当前源码hash标明来源，字段和失效条件见[JSON schemas](json-schemas.md)；无需为取到候选反复扫描或调用诊断。越出工程的`..`、外部文件链接和已知SDK根不借当前同名文件重锚。

`compile_passed`和`compile_failure_fixed`不能替代完整任务验收：保留实际argv/cwd、环境、toolkit来源、目标/TU与依赖闭包证据；任务要求Generate/link/package时覆盖相应阶段。接口、dtype/shape/布局和功能另核；具体补丁可能影响计算、tiling、同步或缓冲时，按原任务约束排除相应行为回归。可用源码、类型或依赖证据；仍有待排除的数值风险时，才执行针对性的合法输入和独立 golden 基准验证。rc=0、日志有源文件名、stdout稳定均不足以判定有效修复。

## SoC与环境故障

显式传准确SKU，例如调用方已确定的910B子型号或950目标；同时读取工程配置、实际构建参数和返回的 `soc_used` / `soc_source` / `soc_mismatch_note`。不要把请求值当成真实采用值，也不因宿主有某NPU就擅自切换目标。 950交叉编译先核后端/头库与pristine正控；实际数值运行另行验收。

仅在安装、解释器或环境能力有疑问时运行环境预检；已有有效检查无需重复。每次命令调用显式设置本次加载目录 `SKILL_DIR`，或使用脚本绝对路径，不假定上一次 shell 变量仍然存在。以下入口使用 `--json`，不接受 `--json-stdout`：

```bash
python3 "$SKILL_DIR/scripts/preflight.py" --json
python3 "$SKILL_DIR/scripts/face_selfcheck.py" --json
```

缺CANN时已有日志与静态诊断仍可提供线索，编译探针不可用。环境不可用、构建未启动和源码编译失败分别报告。只有已有环境可用证据、前置检测误判时才考虑 `ACD_VERIFY_SKIP_ENV_CHECK=1`；它不会让工具链缺失或失败编译算通过。

| 退出码/现象 | 处置 |
|---|---|
| 2 + Unsupported platform | 换Linux Python；Windows只提示支持范围 |
| 2 + engine缺失/Python不可用/output guard | 分别重新取得完整包、修解释器或把所有输出移到包外 |
| 64 | 未知子命令/位置参数不足，读 `run.py --help` |
| ValueError/FileNotFoundError/PermissionError | 按stderr检查参数、真实输入与可写输出；先核已知路径 |
| JSON含error | 诊断工具失败，不是编译类别未命中 |

更多安装问题见[troubleshooting.md](troubleshooting.md)。

## 分享前脱敏

无对外分享时无需执行redact。默认不可逆；需要额外私有词时用 `--custom-words W1 W2`，统计用 `--stats "$STATS_FILE"`。可逆模式 `--reversible` 需cryptography与本地口令；元数据用 `--meta-out "$META_FILE"` 输出到包外。不得把口令写进产物或报告。脱敏本身不授权对外发送。
