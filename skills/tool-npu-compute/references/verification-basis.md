# 技术事实与验证依据

本文件说明字段解释、公式和数据可用性结论应依据哪些信息。遇到目录、参考文档、实际采集数据与代码实现不一致时，先确认差异来源，不组合不一致的信息生成结论。

分析离线报告时优先确认报告生成时的 npu-compute 版本。版本已确认时，以该版本的实现和输出格式为准。版本未知时，可以报告文件中的观测值，但涉及公式、字段支持范围或 `NA` 条件的结论需要标明当前依据和版本不确定性。

## 来源优先级

事实核对按以下顺序取证：

1. 当前 npu-compute 实现和仓库内的 Section、字段及输出目录定义；
2. CANN 官方实现、官方文档和仓库维护的开发规范；
3. 本 Skill 的 `references/`、`assets/` 和目录校验结果；
4. 用户提供的实际采集报告，用于说明本次观测值和文件状态。

低优先级来源不得覆盖高优先级实现事实。无法从可信来源确认的字段含义、硬件上限或性能结论必须标记为未验证，不补造数值或规则。

## 依据范围

### Section 与采集配置

- `npu_tools/npu_compute/src/acl_pti/profiling/range_profiler.cpp` 定义 Section 与采集事件集合的对应关系。
- `npu_tools/npu_compute/src/compute/runtime/section_config.cpp` 处理 Section 配置的读取、去重和有效性检查。

判断 Section 是否受支持时，应同时确认配置入口能够接受该名称，并且采集侧存在对应配置。不使用固定 Event 数字向用户解释指标含义。

### CSV 字段与计算

- `npu_tools/npu_compute/src/compute/report/pmu_csv_writer.cpp` 定义 CSV 的 AIC/AIV 行组织、字段顺序和数值格式。
- `npu_tools/npu_compute/src/compute/pmu/pmu_metric_builder.cpp` 定义字段计算、混合 Kernel 的时长选择和 `NA` 条件。

机器可读字段定义见 `assets/metric-catalog.json`，各 Section reference 提供分析要点。

### HardwareInfo 字段

`npu_tools/npu_compute/src/compute/hardware/` 下的实现分别提供主机信息采集、Device 信息采集、数据类型、JSONL 序列化和文件发布逻辑。`references/hardware-info.md` 与 `assets/hardware-info-catalog.json` 中的类别、字段、类型和含义必须与这些实现保持一致。

### Pipeline 时间线

- `npu_tools/npu_compute/src/compute/runtime/section_config.cpp` 定义 `Pipeline` Section 的启用方式。
- `npu_tools/npu_compute/src/compute/biu/biu_pipeline.cpp` 负责将 BIU 数据转换为完整流水线区间，并写入单个来源片段。
- `npu_tools/npu_compute/src/cli/report/pipe_trace_finalizer.cpp` 校验并合并来源片段，生成最终 `PipeTrace.json`。

`references/pipeline.md` 与 `assets/pipe-trace-catalog.json` 中的顶层字段、事件字段、流水线颜色、轨道标识和时间单位必须与这些实现保持一致。

### 真实采集样本

`tests/fixtures/real-capture/` 保存用于回归验证的真实采集文件。样本用于验证：

- CSV 文件名、表头和字段顺序；
- 数据行列数和字段可用性；
- HardwareInfo 的记录类别、字段顺序和数据类型；
- PipeTrace 的文件头、事件字段、流水线映射、轨道标识和时间范围；
- 检查脚本对真实输入的解析结果。

真实样本只能证明该次采集中的观测值和可用性，不能单独证明其他硬件、负载或采集配置下会得到相同数值。

样本验证属于静态/离线回归，不等价于真实 NPU 端到端采集。只有在具备匹配的 CANN、npu-compute 和 NPU 环境，并得到用户授权后，才能报告真实采集或解包链路已验证。

## 结论边界

- 实际文件用于报告观测值；字段定义和公式以对应代码实现及目录说明为依据。
- `NA` 原因应结合适用 Core、输入计数、分母、频率、时长和实现支持情况判断。
- 性能瓶颈结论还需要负载基线、硬件上限或用户给定阈值；单个字段数值不能自动构成瓶颈结论。
- 代码实现发生变化时，应同步更新 reference、目录表和受影响的测试样本，再运行目录校验和测试。
- 没有可用 NPU 或工具版本无法确认时，验证结论必须限定为静态检查、fixture 回归或 dry-run，不延伸到真实硬件行为。

## 验证方法

从 Skill 目录执行：

```bash
python3 scripts/validate_catalog.py
python3 -m pytest -p no:cacheprovider --confcutdir=tests tests -q
```

目录校验用于检查 reference、目录表和真实样本的一致性；测试用于检查结构、脚本行为和已记录的输入输出契约。
