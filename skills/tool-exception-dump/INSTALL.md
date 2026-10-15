# tool-exception-dump 安装与使用指导

本文说明将 tool-exception-dump 技能加载到三种 AI 助手（opencode、Claude Code、Codex）的安装方式、前置依赖、加载验证与使用示例。本技能是纯知识资产：SKILL.md 承载 34 步诊断主流程与指路规则，references/ 承载各阶段判读规则，evals/ 与 scripts/ 为开发自测资产，不参与运行时安装；安装即把整个目录放到对应助手的技能目录，无需额外注册或编译。

## 前置依赖

- 安装技能本身无依赖。要完整走通诊断流程，需在故障发生环境本地具备 CANN 软件包内置工具链：`msaicerr`（AI Core Error 分析）与 `asys`（故障信息收集）可用，且 Python 不低于 3.7.5。工具用法层细节（CLI 参数、环境准备、三要素排查）本技能不复制，按指路规则指向官方文档（oam-tools docs/zh/msaicerr、docs/zh/asys）。
- 开发自测执行器 scripts/eval_compare.py 仅依赖 Python 标准库（dry-run 模式零外部依赖）。

## 安装方式

三种助手的技能目录与发现机制如下（按各助手官方技能约定）：

### opencode

- 项目级：将本技能目录放置为 `<项目>/.opencode/skills/tool-exception-dump/`。
- 全局：放置到 `~/.config/opencode/skills/tool-exception-dump/`。
- opencode 通过 skill 工具自动发现 SKILL.md，按 frontmatter 的 name 与 description 判定触发；目录放入后即可用，无需额外注册。

### Claude Code

- 项目级：`<项目>/.claude/skills/tool-exception-dump/`（SKILL.md 与 references/、evals/、scripts/ 一并放入）。
- 个人级：`~/.claude/skills/tool-exception-dump/`。
- Claude Code 依据 SKILL.md 的 description 命中触发条件时自动调用本技能，无需手动启用。

### Codex

- 仓库级：`<仓库>/.agents/skills/tool-exception-dump/SKILL.md`（Codex 从当前目录向上扫描 `.agents/skills`，放在仓库根目录可让所有子目录使用）。
- 用户级：`~/.agents/skills/tool-exception-dump/`（部分 Codex 版本同样识别 `~/.codex/skills/`）。
- Codex 启动时扫描技能目录，先只注入 name 与 description 进入上下文，命中后加载完整 SKILL.md；新增技能后需重启 Codex 生效。
- 若所用 Codex 版本未启用 skills 特性，可将本技能的触发与指路说明写入仓库的 AGENTS.md（workspace 指令），使 Codex 将其作为仓库工作区指令加载——此为降级集成路径。

## 加载验证

- opencode：发起与 description 强信号吻合的提问（如"这个报错是不是 AI Core Error"），或查看 skill 工具是否列出 `tool-exception-dump`。
- Claude Code：输入含 aicore error、核错误、结论码 101-107 等触发词的问题，观察是否按 34 步剧本进入 Phase 0 环境与输入检查。
- Codex：启动后使用 `/skills` 浏览（或输入含触发词的问题），确认 `tool-exception-dump` 出现在可用技能列表。

## 使用示例

向已加载技能的助手输入一个 AI Core Error 现场即可，例如：

> 收集目录是 /root/asys_output_20230713074104794，日志里有 aicore error，报错算子是 MatmulAllReduceAddRmsNorm，帮我定位根因。

助手应按本技能剧本推进：先走 P0 环境与输入检查、执行 8 个暂不支持算子名单拦截，再进入场景判定（L0/L1/ffts/SK）、报错行定位、错误类型解码、单算子复现与证据链汇聚，最终输出六段式诊断报告；仅询问 msaicerr/asys 工具用法时，按指路规则指向官方文档（oam-tools docs/zh/msaicerr、docs/zh/asys）。

## 开发自测

适配或修改本技能后，在技能目录执行开发自测（确定性 mock 流，不依赖 opencode）：

```bash
cd skills/tool-exception-dump
python3 scripts/eval_compare.py --mode dry-run
```

11 条评测（RED-GREEN 断言覆盖 34 个诊断步骤）全部通过、退出码 0 即自测通过；失败时读 `<out-dir>/report.md` 定位失败断言并按 SKILL.md/references 修复知识文本，不得为凑绿删除或弱化 evals/evals.json 断言。
