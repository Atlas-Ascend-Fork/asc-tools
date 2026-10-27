# tool-ops-tune 安装与使用指导

本文说明将 tool-ops-tune 套件加载到三种 AI 助手（opencode、Claude Code、Codex）的安装方式、前置依赖、加载验证与使用示例。套件为「单入口 skill + 五子域」形态：`SKILL.md` 是唯一注册入口，五个子域（ops-collect / ops-verdict / ops-mechanism / ops-family / ops-report）以子目录形态由入口按调优阶段路由加载（渐进式披露——判定之前不加载机制卡，证据先行）。安装即把整个 `skills/tool-ops-tune/` 目录放到对应助手的技能目录，无需额外注册或编译。

## 前置依赖

- 安装技能本身无依赖。要完整走通调优流程，需在目标环境具备 CANN 软件包工具链：`msprof`（性能采集）、`npu-smi`（设备查询），且 Python ≥ 3.8 + PyYAML（判定引擎唯一硬依赖）。
- 纯判定 / 判定书解读无需 NPU：已有 msprof 导出的 CSV 即可直接喂 `ops-verdict/scripts/verdict.py`（套件 `fixtures/` 内置样本可直接体验）。
- `agent/` 编排层（五阶段自动调优）需要支持多 agent 的 AI 编程工具；SKILL 子域可脱离 Agent 单独使用（CLI 直跑）。

## 安装方式

三种助手的技能目录与发现机制如下（按各助手官方技能约定）：

### opencode

- 项目级：将本套件目录放置为 `<项目>/.opencode/skills/tool-ops-tune/`。
- 全局：放置到 `~/.config/opencode/skills/tool-ops-tune/`。
- opencode 通过 skill 工具自动发现 SKILL.md，按 frontmatter 的 name 与 description 判定触发；目录放入后即可用，无需额外注册。

### Claude Code

- 项目级：`<项目>/.claude/skills/tool-ops-tune/`（SKILL.md 与子域目录一并放入）。
- 个人级：`~/.claude/skills/tool-ops-tune/`。
- Claude Code 依据 SKILL.md 的 description 命中触发条件时自动调用本套件，无需手动启用。
- 多 agent 编排：将 `agent/AGENTS.md` 作为项目编排规范并入 CLAUDE.md，`agent/agents/` 下两个 subagent 注册到 agents 目录即可启用五阶段自动调优。

### Codex

- 仓库级：`<仓库>/.agents/skills/tool-ops-tune/SKILL.md`（Codex 从当前目录向上扫描 `.agents/skills`，放在仓库根目录可让所有子目录使用）。
- 用户级：`~/.agents/skills/tool-ops-tune/`。
- Codex 启动时扫描技能目录，先只注入 name 与 description 进入上下文，命中后加载完整 SKILL.md；新增技能后需重启 Codex 生效。
- 若所用 Codex 版本未启用 skills 特性，可将套件触发与路由说明写入仓库的 AGENTS.md（workspace 指令），使 Codex 将其作为仓库工作区指令加载——此为降级集成路径。

## 加载验证

- opencode：发起与 description 强信号吻合的提问（如「帮我判定这个算子的瓶颈」），或查看 skill 工具是否列出 `tool-ops-tune`。
- Claude Code：输入含「算子调优 / 瓶颈判定 / Bound 判定 / 性能采集」等触发词的问题，观察是否按五阶段流程进入采集与判定。
- Codex：启动后使用 `/skills` 浏览（或输入含触发词的问题），确认 `tool-ops-tune` 出现在可用技能列表。
- CLI 直跑验证（不依赖助手）：`python3 ops-verdict/scripts/verdict.py --rules ops-verdict/rules/L1-single-core.yaml --csv ops-verdict/fixtures/mte2_bound_a2.csv --soc a2 -o /tmp/v.json`，产出合法判定书即安装完整。
- DB 通道自验（Roadmap #4，可选）：拿一份 npu-compute `.npu-rep` 解包目录跑 `python3 ops-collect/scripts/facts_bridge.py --manifest <解包目录> --soc a5 --print-scalar`——目录特征自动识别 npu-compute 通道；含 `summary.db` 时输出应含 `top_stall_pct`（PcSampling）。
- 回归自检（测试框架）：`python3 ops-verdict/evals/run_evals.py`（执行式判定回归，38 用例）+ `bash ops-verdict/evals/smoke/run_smoke.sh`（离线干跑冒烟，msprof shim 无 NPU 可跑）——退出码 0 才可用；变更规则/脚本后必须先过双绿再交付。

## 使用示例

向已加载套件的助手输入一个调优任务即可，例如：

> 帮我调优这个 gelu 算子，工程在 ./gelu_project，cases.csv 里有 12 个 case，目标是耗时降到 100us 以内

助手应按五阶段闭环执行：精度门禁预检 → 采集与基线固化（ops-collect 路由）→ 分层判定（verdict.py）→ 机制路由与候选方案 → 单变量迭代（每轮 diff_audit + 独立审查 + 同口径对比）→ REPORT 六要素归档，全程产物落 `tuning_artifacts/{operator}/`。

已有 profiling 产物时也可直接问「这个算子为什么慢，给出瓶颈判定和证据」——仅走离线判定与报告解读，不修改源码。
