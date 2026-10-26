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
| <a name="section-pmsampling"></a> `PmSampling`                       | PC/state totals, disassembly and function/inline information       | `full`          |
| <a name="section-resourceconflictratio"></a> `ResourceConflictRatio` | Pipeline wait ratios and Vector resource conflict ratios           | `full`          |

Two predefined sets are available: `basic` contains the seven Sections other than `ResourceConflictRatio` and `PmSampling`, and `full` contains all nine Sections.

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
| `options`             | Tool options. Must precede the target program. See [Options](#options).                                                                                                        |
| `<program>`           | Target program. Can be specified using an absolute path, a relative path, or a command name; when a command name is used, the program must be discoverable through `PATH`.     |
| `[program-arguments]` | Arguments passed unchanged to the target program.                                                                                                                              |
| `[--]`                | Optional separator placed before the target program to separate tool options from the target program and its arguments. Required when the target program name starts with `-`. |
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
| `--replay-mode arg`      | Collect         | `kernel`                 | No         | Kernel replay mode. Only `kernel` is supported and is the default.                                                                                                     |
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

Use `--set full` to collect all supported Sections:

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
| `PipeTrace.json` | `Pipeline` is selected | Pipeline timeline data in Chrome Trace format, viewable with Chrome Trace-compatible tools. |
| `summary.db` | Valid CSV or PmSampling data is available | SQLite database. For the tables it contains, see [SQLite Database](#sqlite-database) |
| `objdump.txt` | When `PmSampling` is selected | Original operator disassembly. Compile with `-lineinfo` to include source line information. |
| `symbolizer.json` | When `PmSampling` is selected | Function and inline information for sampled addresses. |
| `<Section-name>.csv` | Metric data is collected (except for `Pipeline` and `PmSampling`) | Metric data for the Section. |

`Pipeline` uses sampling to generate the pipeline diagram, independent of the number of cores enabled; even when all cores are enabled, the diagram shows data for at most six sampled Groups.

`NA` in a CSV means that no valid value was collected for that metric; other metrics remain available.

### SQLite Database

Collected CSV data is automatically written to `summary.db` in the same result directory. The original CSV files are retained. The database can contain the following tables, depending on the collected results:

| Table                   | Generated when                                                    | Content                                                                                                                                                                                            |
| :---------------------- | :---------------------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ArithmeticUtilization` | Its CSV is imported successfully                                  | [ArithmeticUtilization](#section-arithmeticutilization)                                                                                                                                            |
| `L2Cache`               | Its CSV is imported successfully                                  | [L2Cache](#section-l2cache)                                                                                                                                                                        |
| `Memory`                | Its CSV is imported successfully                                  | [Memory](#section-memory)                                                                                                                                                                          |
| `MemoryL0`              | Its CSV is imported successfully                                  | [MemoryL0](#section-memoryl0)                                                                                                                                                                      |
| `MemoryUB`              | Its CSV is imported successfully                                  | [MemoryUB](#section-memoryub)                                                                                                                                                                      |
| `PcSampling`            | `PmSampling` is selected | Sampling counts for eight wait or stall states, aggregated by business instruction address; see [PC Sampling States](appendix/npu_compute_performance_data.md#pcsampling-pc-sampling-state-totals) |
| `PipeUtilization`       | Its CSV is imported successfully                                  | [PipeUtilization](#section-pipeutilization)                                                                                                                                                        |
| `ResourceConflictRatio` | Its CSV is imported successfully                                  | [ResourceConflictRatio](#section-resourceconflictratio)                                                                                                                                            |
| `FieldUnitMap`          | At least one CSV containing data records is imported successfully | `field_name` stores the column name and `unit` stores its unit, or NULL for a column without a unit.                                                                                               |

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
- Operator invocation: currently, a single invocation of a single operator using `<<<>>>` or aclnn is supported.
- `Pipeline` collection is limited by hardware: only one process on the same device can collect `Pipeline` at a time. If multiple processes collect `Pipeline` concurrently, only one produces output normally; the others fail.
- `PmSampling` collection is limited by hardware: on the same device, `PmSampling` cannot run concurrently with `PmSampling` or `Pipeline` collection in other processes. Concurrent collection may leave samples missing or cause collection to fail.
- `PmSampling` supports sampling only SIMT instructions. Operators that use only SIMD instructions cannot generate the `PcSampling` table; successful results from other Sections are retained.
- When collecting `Pipeline` or `PmSampling` data for a SIMT operator, `noinline` `__aicore__` functions are not supported, including `printf` and `asc_dump`.
- The target program must have execute permission, can be an executable or a script, must run independently without `npu-compute`, and must successfully execute at least one NPU kernel during its run.
- One collection command runs one target program; run separate commands to collect multiple programs. The target program and scripts started by it must not start another `npu-compute` collection. When nested collection is detected, the collection fails and no report is produced.
