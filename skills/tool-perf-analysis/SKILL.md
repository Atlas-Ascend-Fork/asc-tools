---
name: tool-perf-analysis
description: NPU 算子性能报告解读。读取 950 的 npu-compute .npu-rep 报告或非 950 的 msopprof PROF_* 采集目录，抽取事实并调用 tool-ops-tune ops-verdict 规则引擎做瓶颈判定（判定只出自脚本），按判定书 mechanism_refs 路由 tool-ops-tune ops-mechanism 机制卡生成优化候选方案，输出专业性能诊断报告（Markdown + HTML 双格式，含基本信息/问题/证据/优化候选方案）。当用户提供 .npu-rep 报告、msopprof 采集产物并要求"解读性能报告"、"分析这个 profiling 数据"、"算子为什么慢"、"给出瓶颈与优化方案"、"生成性能诊断报告"时触发。
license: CANN-2.0
---

# NPU 算子性能报告解读（tool-perf-analysis）

> **输入**：950（dav-3510）用 npu-compute 采集的 `.npu-rep` 报告；非 950 用 msopprof
> 采集的 `PROF_*` 目录。**输出**：专业性能报告（`report.md` + `report.html` 双格式），
> 内容含基本信息（芯片/算子）、问题（瓶颈判定）、证据（关键指标）、优化候选方案。
>
> **外部引用（硬依赖）**：瓶颈判定与优化机制全部引自
> [tool-ops-tune](https://gitcode.com/cann/asc-tools)（skills/tool-ops-tune）：
> 判定 = `ops-verdict/scripts/verdict.py` 规则引擎（L3 卡间→L2 核间→L1 单核→L0
> 指令分层瀑布，规则-指标绑定契约，未采集不判定）；优化候选方案 =
> `ops-mechanism/references/optimize/*.md` 机制卡（八要素）。
> **本技能只做取数、编排、解释与呈现，不做判定**（判定进脚本、解释进 LLM）。
> 诊断耗时 <30s：脚本链全程 <5s（实测 0.5~0.7s），其余为 LLM 解读预算。

## 0. 总流程（收到采集产物后的执行链）

```
1. 定位输入（§1）：.npu-rep（npu-compute 源）/ PROF_* 目录（msopprof 源）；
   不做采集（采集属 ops-collect / 用户流程），缺产物时按 §6 边界话术停止
2. 跑 scripts/diagnose.py（§2，一条命令完成 ②③④ 全链）：
     ① extract_facts.py   解包 + 事实抽取 + verdict 标量生产
     ② verdict.py         tool-ops-tune 规则引擎判定 → verdict.json
     ③ 机制卡路由         findings[].mechanism_refs → ops-mechanism 机制卡
     ④ render_report.py   → report.md + report.html
3. LLM 解读层（§4）：读取 report.md / diagnose_bundle.json，回答用户追问；
   数值一律引用脚本产物，不另行手算
4. 缺数据/降级路径（§6）：如实呈现缺口与补采建议，不臆造
```

## 1. 输入判定（两源路由）

| 判定 | 输入形态 | 采集工具 | SoC |
|------|---------|---------|-----|
| 源 A | `*.npu-rep` 文件 | npu-compute（950 / dav-3510 专用） | 从 HardwareInfo.jsonl 自动识别（3510→a5） |
| 源 B | `PROF_*` 目录（含 OpBasicInfo.csv） | msopprof（非 950） | **必须 `--soc a2\|a3\|a5` 显式指定**（产物无硬件信息） |

输入数据契约细节（解包产物清单、两源差异、口径规则）见
[references/input-contract.md](references/input-contract.md)。

**硬规则**：
- 只读用户给定产物 + 本技能资源，不启动采集、不上传报告、不修改用户数据；
- `.npu-rep` 解包优先用本技能自带 `scripts/unpack_npu_rep.py`（也兼容
  `npu-compute --import` 的解包目录）；解包失败按 §6 话术停止，不臆造；
- 多 kernel 产物（Op Name 多值）必须 `--kernel-name` 过滤，否则判定无意义；
- SoC 无法确定（msopprof 源漏传 `--soc` / npu-compute 源缺 HardwareInfo）时
  **fail loud 退出**，不静默回落到任一 SoC 标尺——A2 与 A5 的 GM 峰值
  （1600 vs 1400 GB/s）与核数差异足以改变 Bound 结论；
- 无可用标量时不伪造数值送判定引擎（如伪造 task_duration_us=0 会把"缺失"
  伪装成"实测 0"），让 verdict.py 按绑定契约如实输出 INSUFFICIENT_DATA；
- physical_cores 解析顺序：HardwareInfo 芯片实测核数 > soc-spec skus 按
  chip 型号匹配 > reference 典型口径（兜底时来源标注"典型 SKU 口径"警示，
  防多 SKU 设备核利用率错位）。

## 2. 主执行命令（诊断 <30s 的落点）

```bash
# 源 A：950 npu-compute 报告（SoC 自动识别）
python3 scripts/diagnose.py --rep <report.npu-rep> [-o <outdir>] \
    [--ops-tune <tool-ops-tune 路径>] [--kernel-name <kernel>] [--max-findings 3]

# 源 B：msopprof 采集目录（--soc 必填）
python3 scripts/diagnose.py --prof <PROF_dir> --soc a2 [-o <outdir>] [...]
```

产物（默认 `diagnose_out/<报告名>/`）：

| 文件 | 内容 |
|------|------|
| `facts.json` | 事实抽取结果（op/硬件/逐 block 统计/trace 汇总/PcSampling/标量/缺口） |
| `verdict.json` | tool-ops-tune 判定书（findings 证据+反证+置信度+收益区间+机制路由） |
| `diagnose_bundle.json` | facts + verdict + 机制卡正文（报告双格式的同一数据源） |
| `report.md` / `report.html` | 最终报告（双格式，同一数据源——数字不一致=渲染 bug，报 issue 不手改） |

**tool-ops-tune 定位**：`--ops-tune` 显式指定 > `OPS_TUNE_HOME` 环境变量 >
自动探测（`~/.claude/skills/tool-ops-tune` → 同级 `tool-ops-tune` →
`asc-tools*/skills/tool-ops-tune`）。找不到时报错并给出安装指引，**不降级到
"凭经验判定"**——没有规则引擎就没有判定。

**脚本链各步职责**（判定进脚本的边界）：

| 脚本 | 做 | 不做 |
|------|----|----|
| `extract_facts.py` | 解包、列名三代兼容取数、NA 剔除、ratio 双口径归一、soc-spec 峰值换算、verdict 标量生产 | 不判定、不臆造缺失值 |
| （调用）`ops-verdict/scripts/verdict.py` | 规则求值、severity 分档、反证、置信度、路由副作用 | — |
| `diagnose.py` 机制路由段 | `mechanism_refs` → 机制卡路径解析与内嵌；L0 stall 类型补路由（见 references/mechanism-routing.md） | 不改判定结果 |
| `render_report.py` | 五节固定框架双格式渲染、人读化补强（如 stall 规则名补全） | 不产生新数值 |

## 3. 报告固定框架（五节，一字不改）

```markdown
# NPU 算子性能诊断报告
## 1. 基本信息        ← 芯片/架构/核数/算子/类型/Task Duration/Block Dim/频率（降频标注）/采集工具与路径/判定结论
## 2. 瓶颈判定（问题） ← verdict headline + 逐命中条目：规则/证据/阈值溯源/分级注记/典型收益区间/反证；观察档单列
## 3. 证据（关键指标） ← 逐 block pipe 占用表（含串行/并行指纹解读）/ 逐核耗时与长尾离散度 / PcSampling stall 分布 / PipeTrace 采样汇总 / 判定标量表（含来源）
## 4. 优化候选方案（机制路由） ← 每命中一卡：机制方向 + 典型收益区间 + 机制卡正文（details 折叠内嵌 tool-ops-tune 原文）；单变量铁律与实施顺序
## 5. 数据缺口与补采建议 ← missing 逐项（缺什么/为何缺）+ 对应采集工具的补采命令
```

呈现纪律：
- 收益区间标注「典型收益，非本算子预测」（tool-ops-tune 收益引用纪律原文）；
- 单次采集一律标注 single-shot 非稳态；降频（Current < Rated×0.9）标注失真；
- 反证条目如实呈现（"未推翻判定，供复核"），不隐藏不利证据；
- 机制卡正文内嵌 tool-ops-tune 原文（同卡多命中只展开一次）；
- HTML 零外部依赖（内联 CSS，离线可开）；数值不经 hover 也可读（表格视图常驻）。

## 4. LLM 解读层（脚本链之后）

脚本产出报告后，LLM 负责：解释判定书的证据链（为什么是这个瓶颈、为什么不是
别的——用反证与 skipped 说明）、回答追问、机制卡实施建议排序。**边界**：

- 数值一律引用 facts.json / verdict.json / report.md，不另行手算；
- 不输出规则库之外的判定（verdict=INSUFFICIENT_DATA 时如实说证据不足，
  不冒充结论）；
- 不先看机制卡猜瓶颈（证据先行：判定之后才路由机制）；
- 用户要求改代码时，指向 tool-ops-tune 调优主循环（单变量迭代 + diff_audit +
  delta_compare），本技能只出诊断不出实现。

## 5. 外部引用契约（tool-ops-tune 消费点）

| 消费点 | tool-ops-tune 资产 | 引用方式 |
|--------|-------------------|---------|
| 瓶颈判定 | `ops-verdict/scripts/verdict.py` + `rules/L0~L3*.yaml` | subprocess 调用，输入 facts.scalar_str + PipeUtilization.csv，输出 verdict.json |
| 峰值标尺 | `ops-verdict/references/soc-spec.yaml` | extract_facts.py 读取（本技能 references/ 下有同源副本兜底，版本以 tool-ops-tune 为准） |
| 优化候选方案 | `ops-mechanism/references/optimize/*.md`（机制卡八要素）+ `index.md` | diagnose.py 按 mechanism_refs 解析路径并内嵌正文 |
| 路由摘要 | `ops-mechanism` index.md | 本技能 references/mechanism-routing.md 维护本地摘要（降级兜底用，标注非真源） |
| 实施与验证 | `ops-collect`（补采编排）/ `ops-family`（模板）/ snapshot+delta_compare | 报告「数据缺口」节引用补采命令；实施走 tool-ops-tune 调优主循环 |

版本纪律：tool-ops-tune 规则/机制/阈值演进时本技能自动跟随（引用不落副本）；
仅 soc-spec.yaml 有本地副本（网络隔离环境兜底），冲突时以 tool-ops-tune 为准。

## 6. 数据边界与失败话术

统一口径，不得自行发挥：

- **解包失败**：`报告 <path> 解包失败（<原因>）。无法读取明细数据。请确认文件完整或重新采集。`
- **产物缺失**（如仅 Pipeline Section 的 .npu-rep，无逐 block CSV）：如实进入
  数据缺口节 + 补采命令（`npu-compute --set full <program>`）；判定书此时只有
  能跑的规则（L0/summary 侧），不得虚构 L1/L2 结论。
- **msopprof 缺 --soc**：提示必须指定（a2=910B 系 / a3=910_9x 系 / a5=950），
  不猜测芯片。
- **找不到 tool-ops-tune**：报错 + 安装指引（GitCode asc-tools skills/），
  不降级到经验判定。
- **多 kernel 未过滤**：要求 --kernel-name，不产出跨 kernel 平均结论。

禁止行为：
- 用近似值/理论值代替实测值；NA 当 0 统计；
- 把典型收益区间说成对本算子的收益预测；
- 在 verdict 之外新增瓶颈结论（发现规则未覆盖的形态只能作假设并标注缺失指标）；
- 输出 API 签名/Kernel 代码骨架（实施归 tool-ops-tune 调优域）；
- 把 single-shot 说成稳态结论。

## 7. 脚本资产

| 脚本 | 职责 |
|------|------|
| `scripts/diagnose.py` | 主执行器：facts → verdict → 机制卡路由 → 双格式渲染，一条命令 |
| `scripts/extract_facts.py` | 事实抽取器（两源通用；取数+口径换算+标量生产） |
| `scripts/unpack_npu_rep.py` | .npu-rep 二进制容器解包（严格头部/条目校验 + 嵌套 REP 递归） |
| `scripts/render_report.py` | 报告渲染器（Markdown + 单文件 HTML，同一数据源） |
| `evals/run_evals.py` | 执行式回归（4 组用例全链；.npu-rep fixture 本地可用时 64 断言；改动脚本后必须全绿） |
| `evals/fixtures/` | 文本 fixture：msopprof_abs/（A2 msopprof 源 CSV）。`.npu-rep` 二进制 fixture 不入库（仓库禁止二进制文件）；需本地放置（`evals/fixtures/`，已 gitignore）或经 `TOOL_PERF_ANALYSIS_FIXTURES_DIR` / `TOOL_PERF_ANALYSIS_FIXTURE_<KEY>` 环境变量指定，缺失时 A/B 用例 SKIP |

## 8. 验证

```bash
# 全链回归（fixture → 断言 facts/verdict/report 三产物 + fail-loud 防护）
python3 evals/run_evals.py

# 单报告体验（任一 .npu-rep 报告路径）
python3 scripts/diagnose.py --rep <report.npu-rep> \
    --ops-tune <tool-ops-tune 路径> -o /tmp/diag-demo
```
