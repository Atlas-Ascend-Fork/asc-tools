# npu-compute

## Overview

`npu-compute` collects performance data from NPU applications. The tool runs the specified target program, collects [performance data](#performance-data-collection) for the selected metrics, and generates a `.npu-rep` report after successful collection. For an existing report, use `--import` to [unpack](#unpacking-a-report) it into files that can be inspected.

### Metrics

`npu-compute` organizes the metrics it can collect into metric groups (Sections), each containing one category of metrics. A predefined set (Set) contains multiple Sections so that a group of commonly used metrics can be selected at once.

The following Sections are currently supported:

| Section                                                              | Description                                                        | Included in     |
| :------------------------------------------------------------------- | :----------------------------------------------------------------- | :-------------- |
| <a name="section-arithmeticutilization"></a> `ArithmeticUtilization` | Cube FP/INT instruction counts and Cube/Vector active cycle ratios | `basic`, `full` |
| <a name="section-l2cache"></a> `L2Cache`                             | L2 Cache access data                                               | `basic`, `full` |
| <a name="section-memory"></a> `Memory`                               | Main memory and on-chip memory access data                         | `basic`, `full` |
| <a name="section-memoryl0"></a> `MemoryL0`                           | L0 memory access data                                              | `basic`, `full` |
| <a name="section-memoryub"></a> `MemoryUB`                           | Unified Buffer access data                                         | `basic`, `full` |
| <a name="section-pipeline"></a> `Pipeline`                           | Pipeline timeline data, written to `PipeTrace.json`                | `basic`, `full` |
| <a name="section-pipeutilization"></a> `PipeUtilization`             | Compute and pipeline utilization data                              | `basic`, `full` |
| <a name="section-sourcecounter"></a> `SourceCounter` | Instruction execution counts, active SIMT thread counts, SIMT GM Pattern, and SIMT/SIMD VF UB bank conflicts | `full` |
| <a name="section-pmsampling"></a> `PmSampling`                       | PC/state totals, disassembly and function/inline information       | `full`          |
| <a name="section-resourceconflictratio"></a> `ResourceConflictRatio` | Pipeline wait ratios and Vector resource conflict ratios           | `full`          |
| <a name="section-roofline"></a> `Roofline`                           | Peak compute throughput and bandwidth, plus kernel operations, memory traffic, and duration | `full` |
| <a name="section-occupancy"></a> `occupancy` | SIMT Warp activity timeline, written to `SimtWarpOccupancy.json` | Explicit selection |

Two predefined sets are available: `basic` contains the seven Sections other than `PmSampling`, `ResourceConflictRatio`, `SourceCounter`, and `Roofline`, and `full` contains all eleven Sections. Every collection attempts to generate an `Occupancy.json` task/block trace without an additional Section. Use `--section occupancy` to enable the Warp timeline.

For the meaning and calculation method of the fields in each Section, see [Performance Data](appendix/npu_compute_performance_data.md).

## Environment Preparation

Refer to [Quick Start](00_quick_start.md) to prepare the environment. Before use, install a CANN package that matches the target NPU and driver, and run the following command to set the environment variables:

```bash
source ${INSTALL_DIR}/set_env.sh
```

> Replace `${INSTALL_DIR}` with the directory where the CANN software is installed. For a root installation, the default installation path is `/usr/local/Ascend/cann`.

## Command Format and Options

`npu-compute` supports two usage modes: performance data collection and report unpacking.

### Command Format

Collect performance data:

```text
npu-compute [options] [--] <program> [program-arguments]
```

Unpack an existing report:

```text
npu-compute --import <report-file> [--export <directory>]
```

| Placeholder           | Description                                                                                                                                                                    |
| :-------------------- | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `[options]`             | Tool options. Must precede the target program. See [Options](#options).                                                                                                        |
| `[--]`                | Optional separator placed before the target program to separate tool options from the target program and its arguments. Required when the target program name starts with `-`. |
| `<program>`           | Target program. Can be specified using an absolute path, a relative path, or a command name; when a command name is used, the program must be discoverable through `PATH`.     |
| `[program-arguments]` | Arguments passed unchanged to the target program.                                                                                                                              |
| `<report-file>`       | Path to an existing `.npu-rep` report.                                                                                                                                         |
| `<directory>`         | Existing directory that receives the unpacked results.                                                                                                                         |

Everything after the target program is passed to it as arguments.

### Options

| Option                   | Mode            | Input                    | Repeatable | Description                                                                                                                                                            |
| :----------------------- | :-------------- | :----------------------- | :--------- | :--------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `-h`, `--help`           | All             | None                     | Yes        | Shows help information.                                                                                                                                                |
| `--list-sections`        | Query           | None                     | No         | Lists supported Section names.                                                                                                                                         |
| `--list-sets`            | Query           | None                     | No         | Lists `basic`, `full`, and the Sections they contain.                                                                                                                  |
| `--section arg`          | Collect         | Section name             | Yes        | Selects metric groups to collect by name. Names are case-sensitive.                                                                                                    |
| `--set arg`              | Collect         | `basic`, `full`          | Yes        | Selects a predefined set (case-sensitive). Equivalent to specifying each Section in the set with `--section`.                                                          |
| `--replay-mode arg`      | Collect         | `kernel`, `application`   | No         | Replay mode. Defaults to `kernel`; `application` requires an explicit `Communication` Section.                                                                                                     |
| `--no-pack`              | Collect         | None                     | No         | Keeps original result files in a unique directory instead of generating `.npu-rep`.                                                                                    |
| `--cache-control arg`    | Collect         | `all`, `none`            | No         | Controls L2 cache handling before each kernel replay. `all` attempts to flush L2; `none` skips the flush and is the default. |
| `-i arg`, `--import arg` | Unpack          | Report file path         | No         | Imports and unpacks a `.npu-rep` report generated by npu-compute.                                                                                                      |
| `-o arg`, `--export arg` | Collect, Unpack | Report path or directory | No         | For collection, specifies a report file path or an existing directory; for unpacking, specifies an existing directory for the unpacked results. See below for details. |

Combination rules:

- A command can use options from only one mode, as shown in the Mode column.
  - Query: accepts only one query option.
  - Unpack: accepts only `--import` (required) and `--export` (optional).
  - Collect: requires a target program; collect-mode options are optional.
- `--help` takes precedence over other options: when combined with other valid options, only help is shown.

`--export` behaves differently in the two modes:

- Collection: specifies a report file path ending in `.npu-rep` or an existing directory. The parent directory of the report file must already exist, and existing files are not overwritten. If the option is not used, the report is saved in the current directory.
- Collection with `--no-pack`: accepts only an existing parent directory, creates a unique result subdirectory, and generates no `.npu-rep` file. Without `--export`, the current directory is used.
- Unpacking: specifies an existing directory for the unpacked results. The tool creates a unique result subdirectory inside it and does not overwrite existing result directories. If the option is not used, the results are saved in a new result subdirectory under the current directory.

Use the following commands to view help and the currently supported Sections and sets:

```bash
npu-compute --help
npu-compute --list-sections
npu-compute --list-sets
```

## Performance Data Collection

The collection command collects the selected metrics while the target program runs, and generates a `.npu-rep` report after successful collection. After collection finishes, the tool analyzes the results using predefined rules and prints triggered performance hints. See [Performance Data](appendix/npu_compute_performance_data.md) for the detailed rules. For how to specify the target program and pass arguments, see [Command Format](#command-format). For chip support and target program requirements, see [Constraints](#constraints).

### Selecting Metrics

If neither `--set` nor `--section` is specified, `basic` is selected by default.

When the same metric group appears more than once across selections, the tool merges the duplicates and collects each group only once.

### Application Replay for Communication Data

A launcher such as AIN's `ProcessGroup` can fork before its children call `aclInit` and `aclFinalize`. Use the same collection command. Reports from multiple sessions are stored under `pid-<PID>/`, and completion is assessed for the sessions that connect.

```bash
npu-compute --replay-mode application --section Communication --no-pack --export ./results ./app
```

`./results` must already exist. This initial mode collects one communication pass from one process. It cannot be combined with other Sections, `--set`, or explicit `--cache-control`. The `basic` and `full` sets exclude `Communication`. Application mode does not replay kernels and does not impose Kernel mode's single-device or single-Host-thread limits.

Collection starts after a successful `aclrtSetDevice` and skips devices already collecting. Reset is optional: the tool tracks references per device through `aclrtSetDevice`, `aclrtResetDevice`, and `aclrtResetDeviceForce`. Collection stops before the original call for a reset of the last tracked reference or a force reset of a tracked device. After synchronizing device work and finishing the relevant threads, the application must explicitly call `aclFinalize`. The tool starts a collection session for the calling process in the ACL initialization callback. The official ACL finalize notification confirms the profiling tail. The tool drains local data and delivers the result before returning from that notification. If the notification is missing, the tool retains available data and marks the session incomplete. Concurrent device collection requires a Runtime version that supports starting another device while collection is active. The tool does not modify Runtime or synchronize application streams.

Outputs include `HardwareInfo.jsonl`. Each successful launch with an identified name and device also gets `device<id>/op-<encoded-name>/<launch_index>/summary.jsonl`, containing one `OpInfoSummary` record. Repeated calls remain separate; long names use an `instance-<index>` directory. Hardware information is cached while device resources are alive, with `Device Id` on device categories.

Communication collection enables communication timestamps, Task timing, Task L1 timing, AICPU tracing, and one basic PMU pass (808, 1281, 0x422, 0x423, and ALL_BLOCK), without replaying business kernels. Existing summary formulas use these inputs for duration, computational throughput, parallel statistics, and bandwidth. On 950, the raw task fields form a full taskSn, matched to launches within a device and collection epoch. Missing or reused identities and uncertain epochs produce `NA` with `unavailable_reasons`. Communication and compute events from the received records are exported to `CommunicationTrace.json`. The separate application summary `app_summary.jsonl` includes communication and overlap metrics when the required intervals are available. Result manifests use continuation frames to support larger numbers of per-launch artifacts in directory output and packed reports.

The accompanying Runtime must deliver all devices' tail callbacks during `MsprofFinalize`, before the official finalize notification. Normal finalization can succeed when local collection and delivery are healthy, including an empty session without SetDevice. Without the notification, `tailConfirmed=false` and the tool exits with code 3 when the application succeeds. Known Start/Stop, data loss, and processing errors remain failures after finalization. The per-launch summary field `collection_status` records collection completeness.

### Collection Examples

Run `./application` and collect the metrics in the default set basic:

```bash
npu-compute ./application
```

Collect the Memory and PipeUtilization Sections:

```bash
npu-compute \
  --section Memory \
  --section PipeUtilization \
  ./application
```

Use `--set full` to collect the Sections in that set:

```bash
npu-compute --set full ./application
```

Collect the basic set and the ResourceConflictRatio Section:

```bash
npu-compute --set basic --section ResourceConflictRatio ./application
```

Pass the arguments `--input input.bin` to `./application`:

```bash
npu-compute --section PipeUtilization ./application --input input.bin
```

Run the script `./run.sh` through the bash interpreter:

```bash
npu-compute --section PipeUtilization bash ./run.sh
```

Use `--cache-control all` to flush L2 before each kernel replay:

```bash
npu-compute --cache-control all --section L2Cache ./application
```

<a name="simt-warp-occupancy"></a>

### Collecting a SIMT Warp Timeline

Select `--section occupancy` to inspect Warp start times and active durations when analyzing load imbalance or tail blocks. The following example uses the NPUBIT `V100` platform, which defaults to two dies per device and 18 AI Cores per die. To use a different topology, set `NPU_COMPUTE_NPUBIT_DIE_COUNT` and `NPU_COMPUTE_NPUBIT_AICORE_PRE_DIE` to match the device.

```bash
export NPU_COMPUTE_NPUBIT_PLATFORM=V100
npu-compute --section occupancy --section Memory -o ./simt-warp.npu-rep -- ./application
mkdir -p ./restored
npu-compute --import ./simt-warp.npu-rep --export ./restored
```

Open `chrome://tracing` in Chrome and use **Load** to open the extracted `SimtWarpOccupancy.json`. The report groups events by core. The `block` track shows the start and end of a SIMT vector function (VF) invocation. A `warp <ID>` track shows activity for that Warp. The earliest Warp in each invocation is aligned with that invocation's system counter start. Warps without a reliable match appear in a group labeled `(local clock)`. Their timestamps cannot be compared directly across cores.

If the application produces no SIMT records or Warp collection encounters a recoverable error, a mixed collection still delivers the other successful results and warns that the Warp report is missing. If `occupancy` is the only selected Section, a missing Warp report results in a collection failure.

### Report Output

After collection, the tool prints the report path:

```text
npu-compute: report= <report-path>
```

The default report name has the following format:

```text
report_<timestamp-ms>_<random-id>.npu-rep
```

Save the report as `./reports/profile.npu-rep`:

```bash
npu-compute --section PipeUtilization \
  --export ./reports/profile.npu-rep \
  ./application
```

Create an automatically named report in the `./reports` directory:

```bash
mkdir -p ./reports
npu-compute --section Memory --export ./reports ./application
```

Keep the original results directory:

Use `--no-pack` to keep collected CSV, JSON, JSONL and other result files in a unique subdirectory without creating an `.npu-rep` file:

```bash
mkdir -p ./results
npu-compute --section Memory --no-pack --export ./results ./application
```

`--no-pack` takes no value and must appear before the target program. `--no-pack` and `--export` can appear in either order; both must precede the target program. `--export` must specify an existing parent directory, not a report file name. Parent directories are not created automatically. Without `--export`, the current directory is used.

On success, the tool prints:

```text
npu-compute: directory= <parent-directory>/report_<timestamp-ms>_<8-lowercase-hex-random-id>
```

Note: Forcibly terminating the process may leave incomplete directories. If some files fail to be collected during a collection task, delete only the invalid files and retain all other valid files.

## Viewing Reports

A `.npu-rep` report can be viewed directly with a visualization tool (see [Visualization Tool Guide](https://gitcode.com/opdevtools/plugin_release/blob/dev/docs/user_guide.md)), or unpacked to inspect the collected files.

### Report Files

A report can contain the following files:

| File | Generated when | Content |
| :--- | :--- | :--- |
| `HardwareInfo.jsonl` | Every collection | Host and NPU hardware information. |
| `summary.jsonl` | A kernel is executed | Performance summary, [performance hints](appendix/npu_compute_performance_data.md#performancehints), and operator overview. |
| `Occupancy.json` | Attempted by default when collecting | Task and block intervals in Chrome Trace format, with replay identity and raw system counters. |
| `PipeTrace.json` | `Pipeline` is selected | Pipeline timeline data in Chrome Trace format, viewable with Chrome Trace-compatible tools. |
| `SimtWarpOccupancy.json` | `occupancy` is selected and valid Warp records are available | SIMT Warp activity timeline in Chrome Trace format. |
| `summary.db` | When valid CSV data is imported successfully, `PmSampling` collection completes, or `SourceCounter` collection completes | SQLite database. For the tables it contains, see [SQLite Database](#sqlite-database). |
| `objdump.txt` | When `SourceCounter` or `PmSampling` is selected; one file is generated when both are selected | Original operator disassembly. Compile with `-lineinfo` to include source line information. |
| `symbolizer.json` | When `PmSampling` is selected | Function and inline information for sampled addresses. |
| `<Section-name>.csv` | Metric data is collected (except for `Pipeline`, `PmSampling`, `SourceCounter`, `Roofline`, and `occupancy`) | Metric data for the Section. |

**AI Core Occupancy**: Shows the overall task window and when each block is active on its corresponding AI Core (AIC or AIV). All collection modes attempt to generate `Occupancy.json` by default without requiring an additional Section. If only this file fails, the tool logs a warning and delivers the other reports with a successful exit status.

`Pipeline` uses sampling to generate the pipeline diagram, independent of the number of cores enabled; even when all cores are enabled, the diagram shows data for at most six sampled Groups.

`summary.jsonl` stores Roofline data in a record whose `category` is `Roofline`. For field definitions and plotting formulas, see [Roofline data](appendix/npu_compute_performance_data.md#roofline).

`NA` in a CSV means that no valid value was collected for that metric; other metrics remain available.

### SQLite Database

Collected CSV data is automatically written to `summary.db` in the same result directory. The original CSV files are retained. The database can contain the following tables, depending on the collected results:

| Table                   | Generated when                                                    | Content                                                                                                                                                                                            |
| :---------------------- | :---------------------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ArithmeticUtilization` | Its CSV is imported successfully                                  | [ArithmeticUtilization](#section-arithmeticutilization)                                                                                                                                            |
| `FieldUnitMap`          | When a table has unit mappings or at least one CSV with data records is imported successfully | `field_name` stores the column name and `unit` stores its unit, or NULL for a column without a unit.                                                                                               |
| `L2Cache`               | Its CSV is imported successfully                                  | [L2Cache](#section-l2cache)                                                                                                                                                                        |
| `Memory`                | Its CSV is imported successfully                                  | [Memory](#section-memory)                                                                                                                                                                          |
| `MemoryL0`              | Its CSV is imported successfully                                  | [MemoryL0](#section-memoryl0)                                                                                                                                                                      |
| `MemoryUB`              | Its CSV is imported successfully                                  | [MemoryUB](#section-memoryub)                                                                                                                                                                      |
| `PcSampling`            | `PmSampling` is selected | Sampling counts for eight wait or stall states, aggregated by business instruction address; see [PC Sampling States](appendix/npu_compute_performance_data.md#pcsampling-pc-sampling-state-totals). |
| `PipeUtilization`       | Its CSV is imported successfully                                  | [PipeUtilization](#section-pipeutilization)                                                                                                                                                        |
| `ResourceConflictRatio` | Its CSV is imported successfully                                  | [ResourceConflictRatio](#section-resourceconflictratio)                                                                                                                                            |
| `SourceCounter`         | When `SourceCounter` is selected | Execution counts and memory metrics aggregated by original instruction address. See [SourceCounter metrics](appendix/npu_compute_performance_data.md#sourcecounter). |

Database generation depends on a compatible SQLite shared library in the environment. If generation fails, the terminal reports the reason and other successful results can still be packaged.

### Unpacking a Report

Use `--import` to specify an existing `.npu-rep` report. The tool unpacks it into a result directory and prints the directory path.

```bash
npu-compute --import ./reports/profile.npu-rep
```

The tool prints the result directory path:

```text
npu-compute: unpacked= <unpacked-result-directory>
```

The following command unpacks the report into a result subdirectory under `./restored`:

```bash
mkdir -p ./restored
npu-compute --import ./reports/profile.npu-rep --export ./restored
```

The result directory contains the collected files listed in [Report Files](#report-files).

## Constraints

- Chip type: only Ascend 950PR&950DT products is currently supported.
- Programming language: operators to be collected must currently be written in Ascend C.
- Operator invocation in Kernel mode: currently, a single invocation of a single operator using `<<<>>>` or aclnn is supported.
- `Pipeline` collection is limited by hardware: only one process on the same device can collect `Pipeline` at a time. If multiple processes collect `Pipeline` concurrently, only one produces output normally; the others fail.
- `PmSampling` collection is limited by hardware: on the same device, `PmSampling` cannot run concurrently with `PmSampling` or `Pipeline` collection in other processes. Concurrent collection may leave samples missing or cause collection to fail.
- `PmSampling` helps analyze stalls in SIMT instructions during operator execution. Hardware sampling is periodic. If the SIMT code in an operator runs for too short a time, it may finish between sampling instants, leaving no valid samples and an empty `PcSampling` table. Successful results from other Sections are retained.
- `SourceCounter` collects SIMT UB bank conflicts only when SIMT kernel threads directly read or write thread-block-shared UB data through `__ubuf__` pointers. SIMD VF UB bank conflicts are limited to the `asc_load` and `asc_store` vector UB access forms recognized by the current collection mode, which read UB data into vector registers or write vector-register data back to UB.
- When collecting `Pipeline` or `PmSampling` data for a SIMT operator, `noinline` `__aicore__` functions are not supported, including `printf` and `asc_dump`.
- The target program must have execute permission, can be an executable or a script, and must run independently without `npu-compute`. Kernel mode requires at least one successful NPU kernel invocation. Application mode preserves diagnostic artifacts and reports an incomplete collection when no device is collected.
- One collection command runs one target program; run separate commands to collect multiple programs. The target program and scripts started by it must not start another `npu-compute` collection. When nested collection is detected, the collection fails and no report is produced.

## Task and block occupancy trace

```bash
mkdir -p occupancy
npu-compute --no-pack -o occupancy ./application
npu-compute --section PipeUtilization -o occupancy.npu-rep ./application
```

`Occupancy.json` contains Chrome Trace `traceEvents` with complete (`ph: X`) task and block intervals. Open the JSON in Chrome tracing or a compatible trace viewer. Default packed reports and `--import` preserve this JSON. Additional original launches use the existing `collection-p<pid>-<sequence>/Occupancy.json` directory prefix.

Each replay/task/stream has its own process track and its own zero at task start. The display unit is microseconds, converted from the device system-counter frequency queried through `halGetDeviceInfo` (`INFO_TYPE_DEV_OSC_FREQUE`). AI-core MHz and `NPU_COMPUTE_FREQUENCY_MHZ` do not set this clock. Original counters are decimal strings in event arguments so large counter values remain exact.

The Task window uses TaskLog boundaries (`0x00/0x01`). Each block's `blockStartCounter`, `blockEndCounter`, and displayed interval use the start/end system counters from its matching 128-byte block PMU record (`0x29`); `pmuTotalCycles` comes directly from the same record. Original BlockLog boundaries (`0x24/0x25`) remain available as `blockLogStartCounter` and `blockLogEndCounter`, with `blockCounterSource` set to `PMU 0x29`. PMU and BlockLog intervals may differ; a PMU interval does not establish continuous instruction execution throughout that interval.

The trace is generated automatically and shares existing PMU replays when metric Sections are selected. Pipeline/PmSampling/Roofline-only collections add one PMU replay to obtain block counters. Replays are never joined. The trace retains launch, device, replay, task, stream, block, subblock and observed core identities. Each block has its own lane; lane names are `AIC0`, `AIC1`, …, `AIV0`, `AIV1`, …. Interval names include the execution type and original IDs, for example `AIC BLOCK2` and `AIV BLOCK2 SUBBLOCK0`. AIC and AIV numbering each starts at zero and resets for each replay/task/stream. These numbers are display indices, not physical core IDs; the original blockId, subBlockId, coreId32 and coreId128 remain in the event arguments. The Task window comes first, followed by AIC and AIV blocks in ascending numeric blockId/subBlockId order, which determines the display numbering. Conflicting 32/128-byte identity associations or overlapping PMU intervals on the same observed core produce diagnostics and every interval is retained. These labels do not establish Cube/Vector physical grouping.

Collection fails visibly for an unavailable system clock, failed/malformed replay, missing or duplicate BlockLog boundaries or matching `0x29` records, reversed or out-of-task intervals, ambiguous identities, unsupported core types, or overlapping BlockLog intervals on an asserted core. No empty success trace is published. Other valid artifacts remain available when possible.

ACLPTI SDK consumers using `aclptiProfilingDataResult` must rebuild against the updated header. Replay snapshots are appended after the existing fields; the range-profiler configuration struct is unchanged.
