# 高级采集能力

根据实际安装版本和报告来源核对功能与字段。详细定义见[性能数据说明](../../../docs/zh/appendix/npu_compute_performance_data.md)。

当前 docs 约束目标环境为 Ascend 950PR/950DT 系列，待采集算子使用 Ascend C。Kernel 模式支持单算子单次 `<<<>>>` 或 aclnn 调用。`Pipeline` 和 `PmSampling` 受同一设备并发采集限制，采集 SIMT 算子时还需注意 docs 对 `noinline __aicore__` 函数的限制。

## SourceCounter

`SourceCounter` 按原始指令地址汇总指令执行次数、SIMT 活跃线程次数、SIMT GM Pattern，以及 SIMT/SIMD VF 的 UB Bank 冲突。结果写入 `summary.db` 的 `SourceCounter` 表和 `FieldUnitMap` 表。缺失或不适用的值是 SQL `NULL`，实际采集到的零值保留为 `0`。

通过 `address` 关联 `objdump.txt` 的原始指令地址。SIMT UB Bank 冲突只覆盖通过 `__ubuf__` 指针直接访问线程块共享 UB 的情况。SIMD VF Bank 冲突只覆盖工具能识别的 `asc_load` 和 `asc_store` 向量 UB 访问。

## PmSampling

`PmSampling` 对指令地址进行周期采样，汇总八类等待或阻塞状态。结果写入 `summary.db` 的 `PcSampling` 表，并生成 `objdump.txt` 和 `symbolizer.json`。算子运行时间过短时可能没有有效样本，空的 `PcSampling` 不自动表示其他采集失败。

`PmSampling` 与其他进程的 `PmSampling` 或 `Pipeline` 采集存在同设备并发限制。分析时同时查看采样状态、反汇编和函数符号信息。

## Roofline

`Roofline` 在 `summary.jsonl` 中生成 `category` 为 `Roofline` 的记录，按计算类型和数据位宽提供峰值算力 `p_peak`（OP/s）、参考带宽 `b_peak`（GB/s）、计算量 `w`（OP）、耗时 `t`（s）和逻辑 GM 访存量 `q`（B）。当 `q` 和 `t` 都大于零时，按 `I = w / q` 和 `P = w / t` 计算实际点。理论上限为 `min(p_peak, b_peak × 10^9 × I)`。报告只含 `category` 或所需字段为 `NA` 时，报告缺失情况。解释 `q` 时使用逻辑 GM 访问量口径。

## Occupancy

采集默认尝试生成 `Occupancy.json`，记录 Task 和 Block 的 Chrome Trace 时间线。选择 `--section Occupancy` 或 `--set full` 时，还尝试生成 `SimtWarpOccupancy.json`，用于查看 SIMT Warp 的启动时间和活跃时长。

`Occupancy.json` 按 replay、task、stream 将时间对齐到各自的 Task 起点，使用设备系统计数器频率换算为微秒。用事件参数里的原始身份关联 Block，`AIC0`、`AIV0` 等轨道编号仅供显示。`SimtWarpOccupancy.json` 中标记为 local clock 的 Warp 只能在各自时间基准下解释。确认不含 SIMT 的 Kernel 可正常跳过 Warp 采集。可能含 SIMT 却没有有效 Warp 记录时，单独选择 `Occupancy` 会失败，组合采集可保留其他有效产物。详细时间基准见[使用说明](../../../docs/zh/06_npu_compute.md#任务与-block-占用时间线)。

## Application Replay 和 Communication

Application Replay 随应用原有执行采集通信时间戳、Task 时间、Task L1 时间、AICPU Trace、基础 PMU 数据和 HardwareInfo。

成功 launch 的摘要位于 `device<id>/op-<编码名称>/<launch_index>/summary.jsonl`，长名称目录可为 `instance-<编号>`。通信和计算区间写入 `CommunicationTrace.json`，应用级通信及重叠汇总写入 `app_communication_summary.jsonl`。依赖数据缺失时，查看相关字段的 `NA` 和 `unavailable_reasons`。

应用应在业务同步和设备使用线程结束后显式调用 `aclFinalize`，配套 Runtime 须在 finalize 通知前交付尾部数据。缺少通知且应用正常退出时，工具返回 `3`，保留可用数据并标记采集不完整。多进程结果按 `pid-<PID>/` 保存。具体约束见[通信数据采集](../../../docs/zh/06_npu_compute.md#application-replay-通信数据采集)。

## SQLite 指标查询

`summary.db` 位于对应的结果目录，可能包含已入库的 PMU CSV 表、`PcSampling` 和 `SourceCounter`。实际表取决于采集项和入库结果。CSV 列名末尾的单位会拆到 `FieldUnitMap(field_name, unit)` 中，例如 `aiv_time(us)` 入库为 `aiv_time`，单位为 `us`。CSV 中的 `NA` 入库为 SQL `NULL`，采集到的零值仍为 `0`。 [SQLite 数据库说明](../../../docs/zh/06_npu_compute.md#sqlite数据库)

先定位目标算子或采集实例的数据库，以只读方式打开。列出实际表和列，再按用户需要筛选行、选取列或聚合，控制返回行数。查询前确认数据粒度和数值列类型。存在 `block_id`、`sub_block_id` 时可按 Block 和 Sub-block 定位记录，Task 级表可能没有这两列。统计时保留零值并排除 `NULL`。报告使用的相对路径、表、字段和已确认的单位。

例如，在确认 `Memory` 为 Block 级且包含下列字段后，查询执行时间最长的五条 AIV 记录。

```bash
sqlite3 -readonly ./summary.db '.tables'
sqlite3 -readonly ./summary.db 'PRAGMA table_info("Memory");'
sqlite3 -readonly ./summary.db "SELECT unit FROM FieldUnitMap WHERE field_name = 'aiv_time';"
sqlite3 -readonly -header ./summary.db '
  SELECT block_id, sub_block_id, aiv_time
  FROM "Memory"
  WHERE aiv_time IS NOT NULL
  ORDER BY aiv_time DESC, block_id, sub_block_id
  LIMIT 5;'
```

没有 `sqlite3` 命令时，可以使用 Python 标准库 `sqlite3` 以 URI `mode=ro` 打开。查询不到所需数据时检查对应 CSV 或 `summary.jsonl`，以实际文件为准。性能提示和算子概览读取 `summary.jsonl`。

## 其他产物分析

将指令地址关联到 `objdump.txt`，并用 `symbolizer.json` 补充函数及内联信息。编译包含 `-lineinfo` 时可结合反汇编中的源码行号。

对 Occupancy 和通信时间线读取实际 JSON，可用兼容的 Trace 查看器辅助分析。逐行读取应用级通信汇总，按设备和通信模式解释已关联区间。按输出条件判断文件是否预期生成，根据采集状态和终端诊断判断缺失原因。
