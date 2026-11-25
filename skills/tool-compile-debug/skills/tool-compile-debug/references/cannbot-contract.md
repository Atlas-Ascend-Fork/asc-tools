# cannbot-knowledge 编译取证协作

## 职责与调用

宿主直接加载 cannbot-knowledge `knowledge-query` 技能或 cannbot-knowledge `ascendc-api-knowledge-query` 技能；加载后遵循其当前SKILL.md，不经本技能代理搜索。前者提供通用知识检索，后者提供API知识的查找、读卡、来源与SDK头文件取证流程。日志解析、真实环境发现、源码定位、补丁和原配方验证仍在编译闭环中。

一个问题带入首错及原文位置、失败单元、已知版本/build、用户目标与待决项。只补会改变修法的资料。返回后读取正文，记录引用路径/卡片ID、知识checkout、适用版本/平台、实际SDK核验与采用理由。搜索摘要不是完整签名，读到候选不是适用结论；分页必须依上游返回方式读完所需段落。

## 安装准备

使用发行目录外层的自包含脚本安装；支持交互式选择目标客户端（`codex`、`gemini`、`opencode`、`claude`）与安装位置（项目级、全局或自定义目录），也支持非交互参数及只读评测部署：

```bash
bash /发行目录/scripts/install.sh --clients opencode --project /工程目录
bash /发行目录/scripts/install.sh --clients opencode --project /工程目录 \
  --pinned --knowledge-root /已准备的cannbot-knowledge --read-only-knowledge
```

`codex`、`gemini`、`opencode` 共用 `.agents/skills`（全局 `~/.agents/skills`）；`claude` 使用 `.claude/skills`（全局 `~/.claude/skills` 或 `$CLAUDE_CONFIG_DIR/skills`），同时选择时自动建立软链共享。安装固定为纯净版 `tool-compile-debug`（不含外层 `README.md`、`install.sh` 与 `tests/`）与上述两个查询技能，不改变 `AGENTS.md`、`CLAUDE.md` 或 MCP 配置。共享完整知识 checkout 承载治理、索引和卡片，其他维护技能不向宿主启用。

默认采用全局共享目录 `~/.cannbot/cannbot-knowledge`（支持 `--knowledge-root` 覆盖），在线重跑安装命令时默认自动 fast-forward 更新共享知识仓并在需要时重建索引；离线复用传 `--offline`。可复现评测场景使用 `--pinned`（或 `--fetch-root /新目录`）锁定固定 commit，共享只读部署叠加 `--read-only-knowledge`。目标目录存在未受管同名目录或本地修改副本时停止并保留原文件（除非交互确认或显式传 `--force`）。

可选 `--platform` 须为上游注册平台 ID；未知则省略。项目或全局安装会将配置写入对应 `.cannbot/knowledge.env`，只更新知识根和明确提供的平台，保留其他配置。不同评测臂使用各自工程与宿主技能发现根；共享知识在修复会话内保持只读。安装输出只说明准备与索引预检完成，实际加载由宿主发现确认。

## 环境、目标与失败路径

设备探测结果不能覆盖用户编译目标，SKU、产品平台及架构号分开记录。当前查询不支持CANN版本过滤，版本适用性从正文和实际SDK确认，不能把未标版本当跨版本承诺。宿主先遵循API查询技能的精确检索/草稿后续流程，再决定是否需要更宽的检索。

| 情形 | 处理 |
|---|---|
| 技能未发现 | 记录缺失入口，核安装根；继续本地SDK/命令取证 |
| 零命中 | 保留实际范围，按上游流程查询；不判不存在、不删功能 |
| 缺索引、索引过期 | 转安装准备修复；修复会话不改共享checkout |
| 引用资源未下载 | 请求具体资源的准备或查本地SDK；不默认全量拉取 |
| 资料与当前SDK冲突 | 同时保留来源，按失败编译命令的实际声明核实 |
| 无新证据 | 改区分问题、探针或取证来源；不重复调用同一查询 |

## 本地资料准入

保留通用诊断机制、证据状态、验证及恢复方法，不维护API签名、头文件路径、版本迁移表、SoC支持矩阵或错误码百科。确有上游查不到的缺口，补充卡必须写明查询范围、可靠来源、版本/目标适用条件、验证证据与退役条件；上游覆盖后删除重复卡。未经验证的候选不得作为确定性自动修法。
