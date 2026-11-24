---
name: tool-runtime-debug
description: 排查 Ascend C Host Runtime 故障，包括 API 返回值与输出参数、Context/Stream/Event 时序、内存容量、异步完成和资源生命周期。根据已有源码、日志、复现命令和环境信息，从最早可复现的 Host 端失败开始定位。若证据表明最早失败属于编译、AI Core、Tiling、纯精度或多卡通信问题，则停止本专项排查并交接。触发：ACL/ACLNN 返回码异常、输出参数或读回数据错误、Context/Stream/Event 顺序可疑、Memcpy 容量或方向不符、异步完成后读取异常、资源重复或提前释放、Kernel 包来源可疑，以及需要从 Runtime 证据转向其他专项时。
license: CANN-2.0
---

# Ascend C Host Runtime 故障排查

## 适用范围与证据

从最早可复现的 Host 端 CANN Runtime 调用失败开始，检查输出、内存、调用顺序、完成条件和资源所有权是否符合接口约定。API 返回非零值是排查线索；先确认具体调用并保留原始返回值，再提出根本原因假设。

只使用用户提供或明确允许查阅的 Host 源码、实际输出、复现命令、目标环境头文件和环境信息。不得编造构建步骤、API 语义、设备状态或版本覆盖范围。明确区分观察事实与推测，并指出缺失的证据。

若最早失败属于编译、AI Core、Tiling、纯精度或多卡通信问题，保留当前 Runtime 证据，按下文固定取值填写 `next_route`，不绑定具体技能，也不继续跨领域修改。

## 排查与修复流程

1. 记录第一个失败的调用或数据检查点，以及返回值、相关参数、实际输出和产生该结果的命令。
2. 对照目标安装环境的头文件核对调用。根据当前最早失败的证据选择一篇专题；需要环境、API、架构或其他专项能力时，再按下文约定调用对应的 CANNBOT 技能。
3. 一次只保留一个有证据支持、可被推翻的假设，并说明出现什么观察结果就能否定它。单项假设用于限制修改范围，不省略专题要求的前置检查，也不把这些检查推迟到假设被否定之后。
4. 提出能够验证该假设的最小修改，保留验证器、Golden 参考数据、失败分支、比较范围和退出状态的含义。
5. 修改用户源码前，先说明假设和拟修改内容；用户已明确要求直接修改时，无须重复确认。
6. 使用相同的复现命令检查预期数据或任务完成条件，不能只看进程返回码。不得用 `sleep` 代替同步，也不得缩小传输量或比较范围来掩盖结果不一致。
7. 说明修改内容、本次运行证明了什么，以及仍未验证的部分。一次回归成功后停止继续修改；只有新证据改变了最早失败点，才重新定位。

## 按问题选择参考资料

只读取与当前证据相符的专题：

- 返回码、`aclGetRecentErrMsg()` 或最早失败领域不明：[错误证据与分流](references/error-evidence-and-routing.md)。
- 输出值的存储位置、描述对象所有权或指针来源：[输出与所有权](references/output-and-ownership.md)。
- 当前 Context、Stream 对象、Event 记录与等待或完成顺序：[上下文、Stream 与 Event](references/context-stream-event.md)。
- 分配大小、拷贝方向或字节容量：[内存与容量](references/memory-and-capacity.md)。
- 异步完成、日志时间线、回调或 Host 端读取时机：[异步与可观察性](references/async-observability.md)。
- 分配归属、最后消费者或资源清理顺序：[资源生命周期](references/resource-lifecycle.md)。
- `561003`、`561112`、修改未生效或疑似加载旧包：[Kernel 包来源追溯](references/kernel-package-provenance.md)。

每次只读一篇。参考资料用于指导检查，不能据此认定目标版本或设备已经完成测试。

回答官方文档引用问题时，除实际链接和版本外，还须保留该专题要求的核对动作与证据边界，写入 `minimal_change`：目标版本不同时改查对应版本页面，容量与方向核对实际源码参数，释放核对消费者完成，包路径核对当前环境及加载日志。只说明“未验证”不能替代这些具体要求；仅列文档链接也不能证明当前程序正确。

## 使用CANNBOT提供的技能

本技能默认运行环境已经提供 CANNBOT 技能，不负责下载、安装或约定其存放路径。

按 `SKILL.md` frontmatter 的 `name` 查找准确名称，读取匹配技能及当前问题需要的参考页；不要加载全部技能。

技能缺失或重名无法区分时，报告具体缺口；不得声称已调用。只能依据已有的目标头文件、官方资料和用户证据继续，关键依据不足则返回 `UNRESOLVED`。

引用技能不扩大当前任务的设备操作或源码修改权限。

- 需要补充通用错误码、plog 或 Kernel 查找方法时，使用CANNBOT提供的 `ascendc-runtime-debug` 技能来核对；随后仍按本技能的专题边界形成单项假设。不能把当前 `tool-runtime-debug` 当作依赖技能读取。
- 设备不可见或环境配置有疑点时，使用CANNBOT提供的 `ascendc-env-check` 技能来核对设备与 CANN 环境。
- 查询 API 用法、版本语义或官方来源时，使用CANNBOT提供的 `ascendc-docs-search` 技能来检索目标版本资料。
- 当前假设依赖芯片型号、架构代际或能力差异时，使用CANNBOT提供的 `npu-arch` 技能来核对目标平台。
- 排查 Host 初始化、设备选择或核数获取的 API 用法时，使用CANNBOT提供的 `ascendc-api-best-practices` 技能来读取 Host Runtime 规范，并核对目标版本。
- Runtime 修复后只剩精度不符时，使用CANNBOT提供的 `ops-precision-standard` 技能来保留正确的比较统计方式，然后停止 Runtime 修改。
- `561002` 或证据明确指向 Tiling 时，使用CANNBOT提供的 `ascendc-tiling-design` 技能来承接；Runtime 只保留当时的输入、返回码和 workspace 交接证据。
- 证据涉及 Blaze Launcher、RegBase、MC2 或 950 SIMT 专用约束时，分别使用CANNBOT提供的 `ascendc-blaze-best-practice`、`ascendc-regbase-best-practice`、`ascendc-mc2-best-practice` 或 `ascendc-simt-best-practices` 技能来核对；它们不是每次 Runtime 排查的必读项。

每次只读取当前证据需要的技能。专题中的 Blaze、SIMT 引用同样按条件触发；涉及其他领域的根本原因仍遵循停止条件，不因引用而扩大 Runtime 修复范围。

版本经验逐条标注 `applies_to` 和 `verified_on`。`applies_to` 只写官方文档或实测能够支持的范围；`verified_on` 只写可追溯的实际验证环境。只核对官方文档时使用 `not_verified`，与该规则无关的维度使用 `not_applicable`。标签只约束紧随其后的经验，不标注来源条目，也不代表整篇资料或整个 Skill 已验证。

## 只读辅助工具

两个工具均只读，按具体需要使用：

- `scripts/runtime_contract_lint.py`：仅用于已有明确疑点、但阅读源码仍无法确认的接口约定静态检查。
- `scripts/runtime_patch_guard.py`：仅用于明确的修改前后文件对，或文件范围已获授权且可控的工作区。

退出码为零只表示工具的静态检查通过，不代表构建、验证器、NPU 运行或 Runtime 修复通过，也不能直接据此填写 `PASS`。

## 停止条件

出现以下任一情况时，停止当前修复步骤并报告结果或缺口：

- 最早失败无法复现，或缺少验证当前假设所需的源码、命令或实际观察。
- 新证据表明最早失败属于编译、AI Core、Tiling、纯精度或多卡通信。
- 拟议修改会触及验证器、Golden 参考数据、失败分支、比较范围、公开输出字段、枚举或通过阈值。
- 关于目标版本或硬件的结论，没有头文件、官方文档或明确指出的证据支持。
- 已完成一次最小修改及其对应回归。

证据不足时使用 `UNRESOLVED`，不要用漫无边际的搜索或多处猜测性修改代替缺失事实。无须交接或尚不能确定交接领域时，`next_route` 为 `null`；需要交接时，严格使用以下小写取值，不翻译、不改大小写，也不填写具体技能名：

| 交接领域 | `next_route` |
| --- | --- |
| 编译 | `compile` |
| AI Core | `aicore` |
| Tiling | `tiling` |
| 纯精度 | `precision` |
| 多卡通信 | `multi_card` |

交接时若尚未取得下游专项的处理结果，`status` 必须为 `UNRESOLVED`。编译失败或通信超时等原始现象记入 `first_failure`，不能据此把本次交接的状态改为 `FAIL` 或 `UNVERIFIED`；`minimal_change` 说明未做 Runtime 修改及待交接的证据。

## 输出约定

最终回复只能是一个 YAML 对象，直接从 `schema_version` 开始；不得加 Markdown 代码围栏、标题、前后说明或第二个 YAML 文档。包括纯文本排查、资料引用、证据不足和跨域交接在内，都遵守同一格式。

顶层恰好包含以下八个字段，`verification` 恰好包含所列四个字段，不增加 `hypothesis`、否证条件、禁止动作、交接条件等顶层字段。假设、否证依据、限制和交接说明写入 `first_failure` 或 `minimal_change`；实际读过的文件和链接写入 `evidence_refs`。

下面是可解析的结构示例，字段内容须按本次事实填写：

```yaml
schema_version: runtime-result/2.0
status: UNRESOLVED
first_failure: "记录最早失败的证据，以及尚缺的信息。"
hypothesis_id: H001
minimal_change: "未修改；说明下一步最小检查及否证条件。"
verification:
  verifier_integrity: UNVERIFIED
  build: UNVERIFIED
  npu: UNVERIFIED
  stable_runs: 0
next_route: null
evidence_refs: []
```

`first_failure` 和 `minimal_change` 使用字符串、对象或 `null`，不得使用列表；需要多行说明时可用 YAML 块字符串。`hypothesis_id` 为非空字符串，`evidence_refs` 为字符串列表，只列实际查阅的证据。最终回复须符合 [runtime-result/2.0 schema](schemas/runtime-result.schema.json)。

`status` 只能为 `PASS`、`FAIL`、`UNVERIFIED` 或 `UNRESOLVED`；`verification` 的前三项只能为 `PASS`、`FAIL` 或 `UNVERIFIED`。未执行验证时，三项均为 `UNVERIFIED`，`stable_runs` 为整数 `0`，不得把用户描述、参考资料或静态检查当作本次验证通过。`FAIL` 需要实际验证失败的证据；缺少定位依据或下游处理结果时使用 `UNRESOLVED`。

`stable_runs` 如实填写已完成运行次数。只有独立验证器、构建、NPU 执行均通过、实际稳定运行至少三次且无待交接领域时，才能使用 `PASS`。旧执行器的一次确定性回归不能代替此 schema 条件：如实记录 `stable_runs: 1` 和 `status: UNVERIFIED`，在 `minimal_change` 中说明验证范围，不补造次数，也不为凑次数擅自继续执行。

工作区补丁检查仅允许选定Host主源修改，并保护其他受支持源文件及头文件；旧快照缺少source_files时须从未修改的基线重建，不能从修复后的目录补造基线。
