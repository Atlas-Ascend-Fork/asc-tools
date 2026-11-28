---
name: tool-npu-compute
description: 用于 npu-compute 的指标查询、采集、报告解包和结果解释，包括 PerformanceHints 性能提示、SourceCounter、PmSampling、Roofline、SIMT Warp Occupancy 和 Application Replay 通信分析。也适用于明确要求采集 Ascend NPU 指标或解读其 .npu-rep、PipeTrace.json 等产物的请求。
license: CANN-2.0
---

# npu-compute 使用与数据分析

根据用户意图选择流程。结论依据实际命令输出、输入文件和适用的字段定义。

## 触发条件

加载本技能的请求包括以下情况：

- 明确要求使用 `npu-compute`、`NPU-Compute` 或查询其 Set、Section、参数和支持能力。
- 明确要求采集 NPU 性能指标，或解释由 npu-compute 生成的 `.npu-rep`、CSV、`summary.jsonl`、`HardwareInfo.jsonl` 或 `PipeTrace.json`。
- 在 Ascend/NPU 语境下询问应采集哪些指标，例如 `PipeUtilization`、`ArithmeticUtilization`、`ResourceConflictRatio`、缓存或访存指标。
- 在 npu-compute 或 Ascend 性能采集语境下，明确要求 `SourceCounter`、`PmSampling`、`Roofline`、`Occupancy`、`Communication` 或 Application Replay。
- 前文已确认文件来自 npu-compute，当前请求继续分析该文件。

泛化的算子性能分析、瓶颈定位、编译修复和文本整理不单独触发。`Memory`、`Pipeline`、`Set`、`Section`、CSV 等通用词需要结合 npu-compute 语境判断。代码块、日志和历史记录中的偶然提及不改变当前意图。

## 请求分流

- **查询 Set 或 Section。** 运行 `npu-compute --list-sets` 或 `npu-compute --list-sections`。离线查询使用 [Set/Section 查询脚本](scripts/lookup_section.py)，并注明结果来自内置目录。
- **询问用法或只要命令。** 按 [CLI 使用说明](references/cli-usage.md)生成命令和必要说明，不启动目标程序。
- **执行采集或解包报告。** 按 [CLI 使用说明](references/cli-usage.md)构造命令，按[采集与报告流程](references/collection-workflow.md)检查输入、执行并验证产物。
- **解读性能提示。** 对终端 `Performance Hints` 或摘要中的 `PerformanceHints`，按[性能提示说明](references/summary.md#performancehints)解读。
- **分析已有结果。** 检查实际文件，按下文的结果分析流程处理。
- 采集或解读 `SourceCounter`、`PmSampling`、`Roofline`、`Occupancy` 或 `Communication` 时，读取[高级采集能力](references/advanced-sections.md)。
- 目标程序、工作目录、必要参数或输入文件缺失且不能可靠推断时，报告具体缺项。

## 结果分析

1. 检查实际输入，不假定预期文件或指标都存在。
2. 完整报告先运行 [解包结果检查脚本](scripts/inspect_collection.py)，分别识别根级文件和各子结果集合。
3. 查询具体指标、筛选或聚合时，优先以只读方式查询对应结果目录的 `summary.db`。数据库缺失、不可读或缺少所需表、列及数据粒度时，使用有对应数据的 CSV 或摘要，并注明来源。见 [SQLite 指标查询](references/advanced-sections.md#sqlite-指标查询)。
4. CSV 先运行 [CSV 检查脚本](scripts/inspect_csv.py)，再读取对应 Section reference 和 [CSV 指标目录](assets/metric-catalog.json)。当前 CSV 为 Block 级，Task 汇总见 `summary.jsonl`。历史产物按实际表头和版本解释。
5. `summary.jsonl`、`HardwareInfo.jsonl` 和 `PipeTrace.json` 按各自 reference 检查。分析摘要或完整报告时，主动检查每份摘要的 `PerformanceHints`。
6. `objdump.txt`、`symbolizer.json`、`CommunicationTrace.json`、`app_communication_summary.jsonl`、`Occupancy.json` 和 `SimtWarpOccupancy.json` 按[高级采集能力](references/advanced-sections.md)确认模式、文件和可用性。
7. 需要结构化结果时使用[分析报告模板](assets/analysis-report-template.md)。

`NA` 表示不可用或不适用，不等于零。比较数据前匹配 `block_id`、`sub_block_id` 和适用 Core。只有指标定义、硬件基线或用户提供的阈值支持时，才能判断性能瓶颈。`PipeTrace.json` 的 `ts` 和 `dur` 按微秒解释，不同轨道的事件可以重叠。

## 目录输入

明确要求使用本技能处理目录时，只读检查 README、运行脚本、可执行文件和已有报告。已有结果适用时直接分析。需要新增采集时确认目标、参数和当前授权。只有源码时说明构建入口，把源码修改交给用户明确授权的开发流程。

## 失败和权限边界

- 命令、检查脚本或输入文件失败时，报告实际阶段、路径、退出码、错误文本或结构位置，不推断成功结果。
- Set、Section 或字段不存在时，返回当前版本实际列出的候选值。
- 不修改、删除或覆盖用户已有报告。解包使用唯一目录，冲突时报告并停止。
- 不安装软件包、访问未知网络、上传报告或读取凭据。没有 NPU 或工具版本无法确认时，只报告静态检查、fixture 验证或 dry-run 结果。

## 资源路由

字段和格式依据见[技术事实与验证依据](references/verification-basis.md)。

指标定义：

- [指标通用规则](references/metric-foundations.md)
- [ArithmeticUtilization](references/arithmetic-utilization.md)
- [PipeUtilization](references/pipe-utilization.md)
- [ResourceConflictRatio](references/resource-conflict-ratio.md)
- [Memory](references/memory.md)
- [MemoryL0](references/memory-l0.md)
- [MemoryUB](references/memory-ub.md)
- [L2Cache](references/l2-cache.md)
- [数据可用性](references/data-availability.md)
- [summary 字段](references/summary.md)
- [HardwareInfo 字段](references/hardware-info.md)
- [Pipeline 时间线](references/pipeline.md)

目录和检查工具：

- [CSV 指标目录](assets/metric-catalog.json)
- [HardwareInfo 字段目录](assets/hardware-info-catalog.json)
- [PipeTrace 格式目录](assets/pipe-trace-catalog.json)
- [Section 目录](assets/section-catalog.json)
- [summary 字段目录](assets/summary-catalog.json)
- [分析报告模板](assets/analysis-report-template.md)
- [指标查询脚本](scripts/lookup_metric.py)
- [CSV 检查脚本](scripts/inspect_csv.py)
- [解包结果检查脚本](scripts/inspect_collection.py)
- [HardwareInfo 检查脚本](scripts/inspect_hardware_info.py)
- [PipeTrace 检查脚本](scripts/inspect_pipe_trace.py)
- [summary 检查脚本](scripts/inspect_summary.py)
- [目录校验脚本](scripts/validate_catalog.py)

## 验证与维护

维护 reference、目录或真实样本后，在技能目录运行：

```bash
python3 scripts/validate_catalog.py
python3 -m pytest -p no:cacheprovider --confcutdir=tests tests -q
```
