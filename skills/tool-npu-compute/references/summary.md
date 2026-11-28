# summary.jsonl 字段说明

`summary.jsonl` 按行保存 JSON 对象，包括所选 PMU Section 的 Task 指标、所选的 Roofline 数据、性能提示和 Kernel 概览。使用检查脚本读取当前摘要、应用通信的逐次 launch 摘要和受支持的历史摘要。[summary 检查脚本](../scripts/inspect_summary.py) · [性能数据说明](../../../docs/zh/appendix/npu_compute_performance_data.md)

## 记录组成

- 每个所选 PMU Section 对应一条汇总记录，`category` 是 Section 名称。
- Section 记录的指标字段与同名 CSV 一致，但不包含 `block_id` 和 `sub_block_id`。
- 当前 Kernel 报告按名称排序输出 PMU Section 和可选的 `Roofline`，随后输出 `PerformanceHints` 和末尾的 `OpInfoSummary`。记录数为所选 PMU Section 数加 2，选择 Roofline 时再加 1。
- 仅选择 Pipeline 或 PmSampling 时，每个 Kernel 结果也必须生成摘要，包含 `PerformanceHints` 和 `OpInfoSummary`。未执行 Kernel 时不凭空生成摘要。
- 历史报告可仅包含 Section 与末尾的 `OpInfoSummary`。
- 同一个 `category` 在一个文件中只出现一次。
- Application Replay 通信采集的逐次 launch 摘要只有一条 `OpInfoSummary`。使用 `result_unit_id` 和 `launch_index` 定位调用，使用 `collection_status` 查看会话的采集完整性，使用 `unavailable_reasons` 查看各指标缺失原因。

每个 Section 使用当前 Task 的 AIC/AIV 计数计算一条记录。同侧事件在成功的 replay 之间合并。缺少对应侧或必需事件的指标为精确字符串 `"NA"`。当前 Section CSV 保存 Block/sub-block 明细，Task 指标写入 `summary.jsonl`。历史版本可能生成 Task CSV。各指标的含义、单位和不可用条件以对应 Section 字段说明为准。

## PerformanceHints

```json
{"category":"PerformanceHints","hints":["[PH03][AIV] aiv_mte2_ratio（MTE2 流水占比）为 82%，超过 50% 且占比最高；建议检查搬入链路、搬运粒度和数据复用。"]}
```

`hints` 为字符串数组。根据实际摘要记录说明提示状态。

- **有提示。** 结合对应 `OpInfoSummary` 的算子和设备报告命中规则、触发指标及建议检查方向。
- **数组为空。** 工具未生成提示。规则可能未命中，也可能因指标缺失、无效或未采集而跳过。
- **缺少记录。** 报告摘要未提供性能提示，并结合报告版本和采集模式解释，例如历史报告或 Application Replay 的逐次 launch 摘要。

提示适用于 Ascend 950PR&950DT 系列产品。PH01 检查 Vector 占比，PH02 检查 Scalar 占比，PH03 检查 MTE2 占比，PH04 检查平均 GM→UB 搬运量，PH05 检查 ICache 缺失率，PH06 检查核间耗时分布。

PH01–PH05 使用当前 Task 的 Section 汇总值。PH06 按 AIC/AIV 及 sub-block 角色分组，使用 `(最长耗时 - 最短耗时) / 最长耗时`，每类核取有效组最大值。支持两类核的规则分别输出 AIC、AIV 提示，容量单位为 KB（1 KB = 1024 B）。字段不可用和未选择对应 Section 的原因写入 DebugLog。

性能提示提供排查线索。结合实际指标和适用的比较依据判断瓶颈，优化效果通过可比条件下的重新采集确认。完整字段、触发条件、流水比较集合及 PH06 有效样本要求见[性能数据说明](../../../docs/zh/appendix/npu_compute_performance_data.md#performancehints)。

`--import` 保留已存提示，分析时直接读取 `PerformanceHints.hints`。终端输出和摘要包含同一次算子调用的同一提示时，在答复中只展示一次。

## 当前 OpInfoSummary

当前 Kernel 摘要使用精确字符串 `"NA"` 标记不可用值。算子类型优先使用 Kernel 元数据配比，混合算子的 `Mix Block Dim` 表示 Vector 侧逻辑 Block 数。当前频率不可用时回退额定频率，分别使用 AIC 和 AIV 的有效频率计算指标。

算子概览包含 Task 耗时、理论和估算 FP16 算力、逻辑执行对象的并行利用率和均衡度，以及 GM 带宽。并行利用率为逻辑对象平均执行时间除以 Task 耗时，均衡度为平均执行时间除以最长执行时间，两者均以百分比表示。字段、公式和有效样本条件见[当前 OpInfoSummary 字段](../../../docs/zh/appendix/npu_compute_performance_data.md#opinfosummary字段)。

PM Sampling 的 `PmSamplingContexts` 在存在重放上下文时输出。每项使用十进制字符串保存 `replay_id`、`original_entry`、`companion_entry` 和 `pc_offset`。

应用通信逐次 launch 摘要中的 `communication_time`、`compute_communication_overlap_time` 和 `compute_overhead_time` 当前写为 `"NA"`，原因为 `communication_intervals_not_parsed`。设备级的计算、通信和重叠时长见单独的 `app_communication_summary.jsonl`，该文件每个设备输出一条 `DeviceCommunicationSummary`。[应用通信产物](../../../docs/zh/appendix/npu_compute_performance_data.md#application-replay-通信产物)

## Roofline

数据点按 AIV SIMD、AIV SIMT 或 AIC Cube 及位宽标识。数据点包含峰值算力 `p_peak`、带宽参考值 `b_peak`、运算量 `w`、专用重放耗时 `t` 和逻辑 GM 访存量 `q`。没有有效计算量时，Roofline 记录仅包含 `category`。检查脚本在 JSON 输出的 `roofline` 字段或 Markdown 的 Roofline 小节展示数据点。[Roofline 公式与字段](../../../docs/zh/appendix/npu_compute_performance_data.md#roofline)

## 历史摘要

检查脚本按 `OpInfoSummary` 的字段结构选用定义。旧格式缺少四个 FP16 算力字段，可用 `null` 表示缺失值，并行利用率与均衡度以未乘 100 的比值表示。历史 `Mix Block Dim` 可能未提供。字段结构可以识别这些已知格式，同一结构内的计算细节仍取决于采集版本。版本未知时保留实测值，涉及公式时标明所依据的版本或说明版本不确定。[字段目录](../assets/summary-catalog.json)

## 分析方式

1. 按报告版本和模式选择读取方式，检查记录顺序、字段和类型。
2. 使用 Section 汇总值观察 Kernel 整体特征，使用同名 CSV 分析 Block 和 Sub-block 明细。
3. 保留报告的缺失值表示，统计时排除 `"NA"` 和历史报告中的 `null`。
4. 比较多次采集时，先确认 `Op Name`、`Op Type`、`Block Dim`、Device 和频率条件可比。

```bash
python3 scripts/inspect_summary.py <summary.jsonl> --format markdown
```
