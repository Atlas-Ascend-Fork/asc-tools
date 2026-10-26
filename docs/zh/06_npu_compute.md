# npu-compute

## 概述

`npu-compute`是用于采集NPU应用的性能数据的工具。工具运行指定的目标程序，按照用户所选性能指标，进行对应[性能数据的采集](#性能数据采集)，并在采集成功后生成`.npu-rep`报告文件。对于已有报告，可通过`--import`将其[解包](#报告解包)为可查看的采集结果文件。

### 性能指标

`npu-compute`支持采集的性能指标按指标组（Section）组织，每个Section包含一类性能指标。预定义集合（Set）包含多个Section，便于一次选择一组常用指标。

当前支持以下Section：

| Section                                                              | 说明                                                | 所属Set         |
| :------------------------------------------------------------------- | :-------------------------------------------------- | :-------------- |
| <a name="section-arithmeticutilization"></a> `ArithmeticUtilization` | Cube FP/INT指令数与Cube、Vector活跃周期占比         | `basic`、`full` |
| <a name="section-l2cache"></a> `L2Cache`                             | L2 Cache访问数据                                    | `basic`、`full` |
| <a name="section-memory"></a> `Memory`                               | 主存和片上存储访问数据                              | `basic`、`full` |
| <a name="section-memoryl0"></a> `MemoryL0`                           | L0存储访问数据                                      | `basic`、`full` |
| <a name="section-memoryub"></a> `MemoryUB`                           | Unified Buffer访问数据                              | `basic`、`full` |
| <a name="section-pipeline"></a> `Pipeline`                           | 流水时间线数据，输出为`PipeTrace.json`              | `basic`、`full` |
| <a name="section-pipeutilization"></a> `PipeUtilization`             | 计算与流水线利用率数据                              | `basic`、`full` |
| <a name="section-pmsampling"></a> `PmSampling`                       | 指令地址及各采样状态累计计数、反汇编和函数/内联信息 | `full`          |
| <a name="section-resourceconflictratio"></a> `ResourceConflictRatio` | 流水等待率与Vector资源冲突率                        | `full`          |

当前提供`basic`和`full`两个预定义Set：`basic`包含除`ResourceConflictRatio`和`PmSampling`外的7个Section，`full`包含全部9个Section。

各Section的字段含义和计算方法请参见[性能数据说明](appendix/npu_compute_performance_data.md)。

## 环境准备

请参考[快速入门](00_quick_start.md)完成环境准备。使用前，请安装与目标NPU和驱动匹配的CANN软件包，并执行以下命令设置环境变量：

```
source ${INSTALL_DIR}/set_env.sh
```

> `${INSTALL_DIR}`请替换为CANN软件安装后的文件存储路径。以root用户安装为例，安装后文件默认存储路径为：`/usr/local/Ascend/cann`。

## 命令格式与选项

`npu-compute`支持性能数据采集和报告解包两种使用方式。

### 命令格式

采集性能数据：

```text
npu-compute [options] [--] <program> [program-arguments]
```

解包已有报告：

```text
npu-compute --import <report-file> [--export <directory>]
```

| 占位符                | 说明                                                                                                   |
| :-------------------- | :----------------------------------------------------------------------------------------------------- |
| `options`             | 工具选项，必须位于目标程序之前，见[选项](#选项)。                                                      |
| `<program>`           | 目标程序，可以使用绝对路径、相对路径或命令名指定；使用命令名时，该程序必须能够通过`PATH`环境变量找到。 |
| `[program-arguments]` | 原样传递给目标程序的参数。                                                                             |
| `[--]`                | 可选分隔符，放在目标程序前，用于分隔工具选项和目标程序及其参数。目标程序名称以`-`开头时必须添加。      |
| `<report-file>`       | 已有`.npu-rep`报告文件的路径。                                                                         |
| `<directory>`         | 保存解包结果的已有目录。                                                                               |

目标程序后的所有内容均作为程序参数传递。

### 选项

| 选项                     | 适用模式   | 输入            | 可重复 | 说明                                                                                    |
| :----------------------- | :--------- | :-------------- | :----- | :-------------------------------------------------------------------------------------- |
| `-h`、`--help`           | 全部       | 无              | 是     | 显示帮助信息。                                                                          |
| `--list-sections`        | 查询       | 无              | 否     | 列出支持的指标组名称。                                                                  |
| `--list-sets`            | 查询       | 无              | 否     | 列出`basic`、`full`及其包含的Section。                                                  |
| `--section arg`          | 采集       | Section名称     | 是     | 按名称选择要采集的指标组，区分大小写。                                                  |
| `--set arg`              | 采集       | `basic`、`full` | 是     | 选择预定义指标组集合，区分大小写。在采集选择上，等价于分别指定其包含的多个`--section`。 |
| `--replay-mode arg`      | 采集       | `kernel`        | 否     | Kernel重放模式，当前仅支持`kernel`，默认值为`kernel`。                                  |
| `--no-pack`              | 采集       | 无              | 否     | 直接保留原始结果目录，不生成`.npu-rep`。                                                |
| `--cache-control arg`    | 采集       | `all`, `none`   | 否     | 控制每轮 Kernel 重放前的 L2 缓存处理。`all`：尝试清理 L2 缓存；`none`：不主动清理，为默认值。 |
| `-i arg`、`--import arg` | 解包       | 报告文件路径    | 否     | 导入并解包`npu-compute`生成的`.npu-rep`报告。                                           |
| `-o arg`、`--export arg` | 采集、解包 | 报告路径或目录  | 否     | 采集时指定报告文件路径或已有目录，解包时指定保存解包结果的已有目录。具体行为见下文。    |

组合规则：

- 一条命令只能使用“适用模式”列中同一模式的选项。
  - 查询：只接受一个查询选项。
  - 解包：只接受`--import`（必需）和`--export`（可选）。
  - 采集：必须指定目标程序，采集模式选项按需使用。
- `--help`优先于其他选项：与其他有效选项组合时只显示帮助。

`--export`在两种使用方式下的行为不同：

- 采集：指定以`.npu-rep`结尾的报告文件路径或已有目录。报告文件的父目录必须已存在，且不会覆盖已有文件。不使用该选项时，报告保存在当前目录。
- 配合`--no-pack`采集：仅接受已有父目录，在其中创建唯一结果子目录，不生成`.npu-rep`文件；省略`--export`时使用当前目录。
- 解包：指定保存解包结果的已有目录，工具会在其中创建唯一的结果子目录，不会覆盖已有结果目录。不使用该选项时，解包结果保存在当前目录下新建的结果子目录中。

使用以下命令查看帮助和当前支持的指标组、集合：

```bash
npu-compute --help
npu-compute --list-sections
npu-compute --list-sets
```

## 性能数据采集

采集命令在运行目标程序的过程中采集所选指标，采集成功后生成`.npu-rep`报告文件。目标程序的指定要求和参数传递规则见[命令格式](#命令格式)，芯片支持范围和目标程序要求见[约束说明](#约束说明)。

### 选择指标

不指定`--set`和`--section`时，默认选择`basic`。

多次选择中出现重复的指标组时，工具会自动合并，每个指标组只采集一次。

### 采集示例

运行`./application`，采集默认集合basic中的指标：

```bash
npu-compute ./application
```

采集Memory和PipeUtilization指标组：

```bash
npu-compute \
  --section Memory \
  --section PipeUtilization \
  ./application
```

使用`--set full`采集全部支持的Section：

```bash
npu-compute --set full ./application
```

采集basic集合和ResourceConflictRatio指标组：

```bash
npu-compute --set basic --section ResourceConflictRatio ./application
```

向`./application`传递参数`--input input.bin`：

```bash
npu-compute --section PipeUtilization ./application --input input.bin
```

通过bash解释器运行脚本`./run.sh`：

```bash
npu-compute --section PipeUtilization bash ./run.sh
```

使用`--cache-control all`，在每轮 Kernel 重放前主动清理 L2 缓存：

```bash
npu-compute --cache-control all --section L2Cache ./application
```

### 报告输出

采集完成后，工具会在终端输出报告路径：

```text
npu-compute: report= <report-path>
```

默认报告的名称格式如下：

```text
report_<timestamp-ms>_<random-id>.npu-rep
```

将报告保存为`./reports/profile.npu-rep`：

```bash
npu-compute --section PipeUtilization \
  --export ./reports/profile.npu-rep \
  ./application
```

在`./reports`目录中生成自动命名的报告：

```bash
mkdir -p ./reports
npu-compute --section Memory --export ./reports ./application
```

直接保留原始结果目录：

使用`--no-pack`在指定父目录内创建唯一的结果子目录，保存本次CSV、JSON、JSONL等采集结果，不生成`.npu-rep`：

```bash
mkdir -p ./results
npu-compute --section Memory --no-pack --export ./results ./application
```

`--no-pack`不带值，工具选项仍应放在目标程序之前。`--no-pack`与`--export`的先后顺序不限，两者都必须放在目标程序之前。`--export`必须指向已有目录，不能指定报告文件名，也不会自动创建父目录；省略时使用当前目录。

成功后输出：

```text
npu-compute: directory= <parent-directory>/report_<timestamp-ms>_<8-lowercase-hex-random-id>
```

注意：强制结束进程可能留下不完整目录。当采集任务中存在部分文件采集失败时，仅删除无效文件，保留其他有效文件。

## 查看报告

`.npu-rep`报告可直接使用可视化工具查看（参见[可视化工具使用说明](https://gitcode.com/opdevtools/plugin_release/blob/dev/docs/user_guide.md)），也可解包后查看其中的采集文件。

### 报告文件

报告可包含以下文件：

| 文件                 | 生成条件                                             | 内容                                                               |
| :------------------- | :--------------------------------------------------- | :----------------------------------------------------------------- |
| `HardwareInfo.jsonl` | 每次采集时生成                                       | 主机和NPU硬件信息。                                                |
| `summary.jsonl`      | 执行Kernel时生成                                     | 性能汇总及算子概览。                                               |
| `PipeTrace.json`     | 选择`Pipeline`时生成                                 | 流水时间线数据，Chrome Trace格式，可使用Chrome Trace兼容工具查看。 |
| `summary.db`         | 存在有效CSV或PmSampling采样数据时                    | SQLite数据库，所含数据表见[SQLite数据库](#sqlite数据库)            |
| `objdump.txt`        | `PmSampling`采集到指令地址及状态计数时生成 | 原始算子程序的反汇编。算子使用`-lineinfo`编译时，可显示源码行号。  |
| `symbolizer.json`    | `PmSampling`采集到指令地址及状态计数时生成 | 采样地址对应的函数和内联调用信息。                                 |
| `<Section-name>.csv` | 采集到指标数据时生成（`Pipeline`和`PmSampling`除外） | 该Section的指标数据。                                              |

`Pipeline` Section基于采样机制生成流水图，与用户启用的核数无直接关系；即使启用全部核，流水图最多展示6个采样Group的数据。

CSV中的`NA`表示该项指标未采集到有效值，其他指标仍可正常查看。

各文件的字段含义、计算方法及CSV与summary的关系，请参见[性能数据说明](appendix/npu_compute_performance_data.md)。

### SQLite数据库

采集生成的CSV自动写入同一结果目录的`summary.db`，原CSV继续保留。数据库包含以下表，实际生成的表取决于采集结果：

| 数据表                  | 生成条件                        | 内容                                                                                                                                      |
| :---------------------- | :------------------------------ | :---------------------------------------------------------------------------------------------------------------------------------------- |
| `ArithmeticUtilization` | 对应CSV入库成功                 | [ArithmeticUtilization](#section-arithmeticutilization)                                                                                   |
| `L2Cache`               | 对应CSV入库成功                 | [L2Cache](#section-l2cache)                                                                                                               |
| `Memory`                | 对应CSV入库成功                 | [Memory](#section-memory)                                                                                                                 |
| `MemoryL0`              | 对应CSV入库成功                 | [MemoryL0](#section-memoryl0)                                                                                                             |
| `MemoryUB`              | 对应CSV入库成功                 | [MemoryUB](#section-memoryub)                                                                                                             |
| `PcSampling`            | 选择`PmSampling` | 按业务指令地址汇总八种等待或阻塞状态的采样计数，见[PC采样状态说明](appendix/npu_compute_performance_data.md#pcsamplingpc采样状态累计计数) |
| `PipeUtilization`       | 对应CSV入库成功                 | [PipeUtilization](#section-pipeutilization)                                                                                               |
| `ResourceConflictRatio` | 对应CSV入库成功                 | [ResourceConflictRatio](#section-resourceconflictratio)                                                                                   |
| `FieldUnitMap`          | 至少一个含数据记录的CSV成功入库 | `field_name`保存字段名，`unit`保存单位；无单位时为NULL。                                                                                  |

数据库生成依赖环境中的SQLite动态库。生成失败时，终端提示原因，其他成功结果仍可生成报告。

### 报告解包

使用`--import`指定已有的`.npu-rep`报告，工具将其解包到结果目录并输出该目录路径。

```bash
npu-compute --import ./reports/profile.npu-rep
```

工具输出解包结果目录的路径：

```text
npu-compute: unpacked= <unpacked-result-directory>
```

以下命令将报告解包到`./restored`目录下的结果子目录：

```bash
mkdir -p ./restored
npu-compute --import ./reports/profile.npu-rep --export ./restored
```

解包结果目录中包含[报告文件](#报告文件)列出的采集文件。

## 约束说明

- 芯片类型：当前仅支持Ascend 950（dav-3510）。
- 编程语言：待采集的算子当前仅支持Ascend C。
- 算子调用方式：当前支持通过`<<<>>>`或aclnn方式进行单算子单次调用。
- `Pipeline`采集受硬件限制：同一Device上只能有一个进程采集`Pipeline`；多个进程同时采集时，只有一个进程能正常输出，其他进程采集失败。
- `PmSampling`采集受硬件限制：同一Device上，`PmSampling`不能与其他进程的`PmSampling`或`Pipeline`采集同时执行；并发采集可能导致采样数据缺失或采集失败。
- `PmSampling`仅支持对SIMT指令进行采样；仅使用SIMD指令的算子无法生成`PcSampling`表，其他采集项的成功结果仍保留。
- 采集SIMT算子的`Pipeline`或`PmSampling`数据时，不支持使用`noinline`的`__aicore__`函数，比如`printf`和`asc_dump`。
- 目标程序必须具有执行权限，可以是可执行文件或脚本，能够在未使用`npu-compute`时独立运行，并在运行过程中至少成功执行一次NPU核函数。
- 一条采集命令只运行一个目标程序；需要采集多个程序时，分别执行多条命令。目标程序及其启动的脚本不能再次运行`npu-compute`，检测到嵌套采集时本次采集失败且不会生成报告。
