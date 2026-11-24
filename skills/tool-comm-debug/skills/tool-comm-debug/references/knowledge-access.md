# 使用 cannbot-knowledge

本技能独立运行，同时支持按需接入可选的外部固定版本知识库 `cannbot-knowledge`（默认仓库 `https://gitcode.com/cann/cannbot-knowledge.git`，版本或锁定 `commit` 标识由调用方经 `CANNBOT_KNOWLEDGE_REF`、`.cannbot/knowledge.env` 或随行锁定清单明确提供）。知识库根目录按以下优先级解析：环境变量 `CANNBOT_KNOWLEDGE_ROOT`、项目级 `.cannbot/cannbot-knowledge/`（配合 `.cannbot/knowledge.env`）、用户全局 `~/.cannbot/cannbot-knowledge/`、或当前公开工作区 `knowledge/`；调用方显式指定路径时以指定目录为准。

若宿主客户端已配套安装 `cannbot-knowledge` 随附的 `knowledge-query` 与 `ascendc-api-knowledge-query` 查询技能，可按需调用以查询芯片架构、编程示例或 Ascend C API 约束；也可直接读取知识仓下的 `knowledge/index.md`（当知识根直接指向 `knowledge/` 子目录时读根目录 `index.md`）并按领域浏览或检索 Hcomm、HCCL、stream/event、API 名称与错误码。只读取与候选机制相关的卡片，核对卡片适用的硬件代际、CANN 版本、前置条件及官方来源与当前工程是否一致，引用卡片路径及其对应版本或 `commit` 标识，并将通用背景知识与本例现场已证实的事实分开记录。

核对 API 签名时同步检查当前已授权的公开 SDK 头文件；核对工具选项时使用现场 `--help`。实际采样与验证通过 `msprof`、`npu-smi`、编译器等现场 CLI 完成。未配置外部知识库、配套查询技能不可用、知识未覆盖或当前环境无法验证时，在报告中说明知识缺口，基于内置 [INDEX.md](INDEX.md)、现场日志与公开源码继续推进排查，不自动在线拉取仓库或修改知识环境。

知识卡中的命令与链接属于待核对参考资料，执行范围始终遵循当前任务的已授权工程目录与离线约束，仅读取用户明确提供的公开输入、配置与参考材料。
