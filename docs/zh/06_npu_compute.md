# npu-compute

## 概述

`npu-compute`是用于采集NPU应用的性能数据的工具。工具运行指定的目标程序，按照用户所选性能指标，进行对应[性能数据的采集](#性能数据采集)，并在采集成功后生成`.npu-rep`报告文件。对于已有报告，可通过`--import`将其[解包](#报告解包)为可查看的采集结果文件。

### 性能指标

`npu-compute`支持采集的性能指标按指标组（Section）组织，每个Section包含一类性能指标。预定义集合（Set）包含多个Section，便于一次选择一组常用指标。

当前支持以下Section：

| Section                                                                | 说明                                                | 所属Set             |
| :--------------------------------------------------------------------- | :-------------------------------------------------- | :------------------ |
| <a name="section-arithmeticutilization"></a> `ArithmeticUtilization` | Cube FP/INT指令数与Cube、Vector活跃周期占比         | `basic`、`full` |
| <a name="section-l2cache"></a> `L2Cache`                             | L2 Cache访问数据                                    | `basic`、`full` |
| <a name="section-memory"></a> `Memory`                               | 主存和片上存储访问数据                              | `basic`、`full` |
| <a name="section-memoryl0"></a> `MemoryL0`                           | L0存储访问数据                                      | `basic`、`full` |
| <a name="section-memoryub"></a> `MemoryUB`                           | Unified Buffer访问数据                              | `basic`、`full` |
| <a name="section-pipeline"></a> `Pipeline`                           | 流水时间线数据，输出为`PipeTrace.json`            | `basic`、`full` |
| <a name="section-pipeutilization"></a> `PipeUtilization`             | 计算与流水线利用率数据                              | `basic`、`full` |
| <a name="section-sourcecounter"></a> `SourceCounter` | 指令执行次数、SIMT活跃线程次数、SIMT GM Pattern及SIMT/SIMD VF指令的UB Bank冲突 | `full` |
| <a name="section-pmsampling"></a> `PmSampling`                       | 指令地址及各采样状态累计计数、反汇编和函数/内联信息 | `full`          |
| <a name="section-resourceconflictratio"></a> `ResourceConflictRatio` | 流水等待率与Vector资源冲突率                        | `full`          |
| <a name="section-roofline"></a> `Roofline`                           | 理论算力和带宽，以及Kernel计算量、访存量和耗时       | `full`          |
| <a name="section-occupancy"></a> `occupancy` | SIMT Warp活动时间线，输出为`SimtWarpOccupancy.json` | 单独选择 |

当前提供`basic`和`full`两个预定义Set：`basic`包含除`PmSampling`、`ResourceConflictRatio`、`SourceCounter`和`Roofline`外的7个Section，`full`包含全部11个Section。每次采集均尝试生成`Occupancy.json`任务与block时间线，无需额外Section。使用`--section occupancy`显式启用Warp时间线。

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

| 占位符                  | 说明                                                                                                     |
| :---------------------- | :------------------------------------------------------------------------------------------------------- |
| `options`             | 工具选项，必须位于目标程序之前，见[选项](#选项)。                                                         |
| `<program>`           | 目标程序，可以使用绝对路径、相对路径或命令名指定；使用命令名时，该程序必须能够通过`PATH`环境变量找到。 |
| `[program-arguments]` | 原样传递给目标程序的参数。                                                                               |
| `[--]`                | 可选分隔符，放在目标程序前，用于分隔工具选项和目标程序及其参数。目标程序名称以`-`开头时必须添加。      |
| `<report-file>`       | 已有`.npu-rep`报告文件的路径。                                                                         |
| `<directory>`         | 保存解包结果的已有目录。                                                                                 |

目标程序后的所有内容均作为程序参数传递。

### 选项

| 选项                         | 适用模式   | 输入                | 可重复 | 说明                                                                                              |
| :--------------------------- | :--------- | :------------------ | :----- | :------------------------------------------------------------------------------------------------ |
| `-h`、`--help`           | 全部       | 无                  | 是     | 显示帮助信息。                                                                                    |
| `--list-sections`          | 查询       | 无                  | 否     | 列出支持的指标组名称。                                                                            |
| `--list-sets`              | 查询       | 无                  | 否     | 列出`basic`、`full`及其包含的Section。                                                        |
| `--section arg`            | 采集       | Section名称         | 是     | 按名称选择要采集的指标组，区分大小写。                                                            |
| `--set arg`                | 采集       | `basic`、`full` | 是     | 选择预定义指标组集合，区分大小写。在采集选择上，等价于分别指定其包含的多个`--section`。         |
| `--replay-mode arg`        | 采集       | `kernel`、`application` | 否     | 重放模式，默认值为`kernel`。`application`须显式选择`Communication`。                                        |
| `--no-pack`                | 采集       | 无                  | 否     | 直接保留原始结果目录，不生成`.npu-rep`。                                                        |
| `--cache-control arg`      | 采集       | `all`, `none`   | 否     | 控制每轮 Kernel 重放前的 L2 缓存处理。`all`：尝试清理 L2 缓存；`none`：不主动清理，为默认值。 |
| `-i arg`、`--import arg` | 解包       | 报告文件路径        | 否     | 导入并解包`npu-compute`生成的`.npu-rep`报告。                                                 |
| `-o arg`、`--export arg` | 采集、解包 | 报告路径或目录      | 否     | 采集时指定报告文件路径或已有目录，解包时指定保存解包结果的已有目录。具体行为见下文。              |

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

采集命令在运行目标程序的过程中采集所选指标，采集成功后生成`.npu-rep`报告文件。采集结束后，工具会根据预设规则分析结果，并输出相关性能提示；详细规则见[性能数据说明](appendix/npu_compute_performance_data.md)。目标程序的指定要求和参数传递规则见[命令格式](#命令格式)，芯片支持范围和目标程序要求见[约束说明](#约束说明)。

### 选择指标

不指定`--set`和`--section`时，默认选择`basic`。

多次选择中出现重复的指标组时，工具会自动合并，每个指标组只采集一次。

### Application Replay 通信数据采集

父进程可以先 fork，由各子进程分别调用 `aclInit` 和 `aclFinalize`，例如 AIN 的 `ProcessGroup`。各进程沿用同一条采集命令。多份报告按 `pid-<PID>/` 保存，采集完整性以实际连接的会话为准。

```bash
npu-compute --replay-mode application --section Communication --no-pack --export ./results ./app
```

`./results`须为已有目录。当前仅支持单个采集进程、单轮通信数据采集，不能与其他 Section、`--set` 或显式`--cache-control`组合；`basic`和`full`不包含`Communication`。Application模式不重放Kernel，不要求Kernel模式的单设备、单Host线程约束。

每次成功的`aclrtSetDevice`后启动该设备采集；同一设备处于采集中时不重复启动。Reset可选：工具按设备跟踪经`aclrtSetDevice`、`aclrtResetDevice`和`aclrtResetDeviceForce`发生的引用变化；释放最后一个引用的普通Reset或有引用的Force Reset在调用原接口前停止对应设备。用户在相关业务同步和设备使用线程结束后必须显式调用`aclFinalize`。工具在 ACL 初始化回调中为当前进程建立采集会话。在 ACL finalize 通知确认尾部后，工具在通知返回前完成本地解码、报表生成和结果交付。缺少 finalize 通知时，工具保留可用数据并报告采集不完整。多设备共存依赖Runtime支持在已启动设备采集后继续启动其他设备；工具不修改Runtime，也不主动同步应用的Stream。

输出`HardwareInfo.jsonl`，并为每个能确定名称和设备的成功launch生成`device<id>/op-<编码名称>/<launch_index>/summary.jsonl`，其中仅包含一条`OpInfoSummary`。同名多次调用分别保留，长名称使用`instance-<编号>`目录。各设备硬件信息在设备资源存活时缓存，设备分类记录带`Device Id`。

通信采集固定启用通信时间戳、Task时间、Task L1时间、AICPU Trace和单轮基础PMU（808、1281、0x422、0x423，以及ALL_BLOCK），不重放业务Kernel。这些数据用于现有摘要的时长、计算量、并行统计和带宽公式。950原始任务字段拼成完整taskSn，并按device和采集周期与launch关联；缺失、复用或无法确认周期时，对应指标为`NA`并附`unavailable_reasons`。已收到的数据经解析后生成`CommunicationTrace.json`，展示通信与计算时间块。应用级`app_summary.jsonl`根据已关联的区间汇总通信与重叠指标。结果清单分批传输，目录输出和报告打包均支持较多算子实例。

配套Runtime须保证`MsprofFinalize`在正式finalize通知前通过callback交付全部设备尾数据；正常finalize且本地采集和交付无错误时可成功，无SetDevice的正常finalize可交付完整空会话。未收到通知时`tailConfirmed=false`，应用成功时工具退出码为3；已知Start/Stop错误、丢失或处理失败不会被最终通知清除。逐算子摘要中的`collection_status`记录会话采集完整性。

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

使用`--set full`采集该集合中的Section：

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

使用`--cache-control all`，在每轮Kernel重放前主动清理L2缓存：

```bash
npu-compute --cache-control all --section L2Cache ./application
```

<a name="simt-warp-occupancy"></a>

### 采集SIMT Warp时间线

用`--section occupancy`观察各Warp的启动时间和活跃时长，分析Warp间的负载差异和尾块处理。以下示例使用NPUBIT `V100`平台，默认每个Device有2个Die，每个Die有18个AI Core。其他拓扑请按设备实际配置`NPU_COMPUTE_NPUBIT_DIE_COUNT`和`NPU_COMPUTE_NPUBIT_AICORE_PRE_DIE`。

```bash
export NPU_COMPUTE_NPUBIT_PLATFORM=V100
npu-compute --section occupancy --section Memory -o ./simt-warp.npu-rep -- ./application
mkdir -p ./restored
npu-compute --import ./simt-warp.npu-rep --export ./restored
```

解包后，在Chrome地址栏打开`chrome://tracing`，选择 **Load**，加载`SimtWarpOccupancy.json`。报告按核分组。`block`轨道表示该核上一次SIMT向量函数（VF）调用的起止区间，`warp <ID>`轨道表示对应Warp的活动区间。每轮最早启动的Warp对齐到本轮VF的系统计数器起点。无法可靠配对的Warp显示在带`(local clock)`的分组中，这些时间不能跨核直接比较。

当算子无SIMT记录或Warp采集遇到可恢复错误时，混合采集仍会交付其他成功的结果，Warp报告缺失会提示告警。只选择`occupancy`而没有生成Warp报告时，工具以采集失败退出。

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

| 文件 | 生成条件 | 内容 |
| :--- | :--- | :--- |
| `HardwareInfo.jsonl` | 每次采集时生成 | 主机和NPU硬件信息。 |
| `summary.jsonl` | 执行Kernel时生成 | 性能汇总、[性能提示](appendix/npu_compute_performance_data.md#performancehints)及算子概览。 |
| `Occupancy.json` | 采集时默认尝试生成 | 任务与block区间，Chrome Trace格式，保留replay身份与原始系统计数器。 |
| `PipeTrace.json` | 选择`Pipeline`时生成 | 流水时间线数据，Chrome Trace格式，可使用Chrome Trace兼容工具查看。 |
| `SimtWarpOccupancy.json` | 选择`occupancy`且采集到有效Warp记录时 | SIMT Warp活动时间线，采用Chrome Trace格式。 |
| `summary.db` | 有效CSV成功入库，或完成`PmSampling`采集，或完成`SourceCounter`采集时 | SQLite数据库，所含数据表见[SQLite数据库](#sqlite数据库)。 |
| `objdump.txt` | 选择`SourceCounter`或`PmSampling`时生成；同时选择时生成一份 | 原始算子程序的反汇编。算子使用`-lineinfo`编译时，可显示源码行号。 |
| `symbolizer.json` | 选择`PmSampling`时生成 | 采样地址对应的函数和内联调用信息。 |
| `<Section-name>.csv` | 采集到指标数据时生成（`Pipeline`、`PmSampling`、`SourceCounter`、`Roofline`和`occupancy`除外） | 该Section的指标数据。 |

**AI Core Occupancy**：展示任务的整体时间窗口，以及各 block 在对应 AI Core（AIC/AIV）上的活跃时间区间。各类采集任务默认尝试生成 `Occupancy.json` 报告，无需额外配置 Section。如果只有这个文件生成失败，工具会记录告警，其余报告照常交付，命令正常退出。

`Pipeline` Section基于采样机制生成流水图，与用户启用的核数无直接关系；即使启用全部核，流水图最多展示6个采样Group的数据。

`Roofline`数据保存在`summary.jsonl`中`category`为`Roofline`的记录里。字段含义和绘图方法见[Roofline数据说明](appendix/npu_compute_performance_data.md#roofline)。

CSV中的`NA`表示该项指标未采集到有效值，其他指标仍可正常查看。

### SQLite数据库

采集生成的CSV自动写入同一结果目录的`summary.db`，原CSV继续保留。数据库包含以下表，实际生成的表取决于采集结果：

| 数据表                  | 生成条件                        | 内容                                                                                                                                      |
| :---------------------- | :------------------------------ | :---------------------------------------------------------------------------------------------------------------------------------------- |
| `ArithmeticUtilization` | 对应CSV入库成功                 | [ArithmeticUtilization](#section-arithmeticutilization)                                                                                   |
| `FieldUnitMap`          | 存在带单位映射的数据表，或至少一个含数据记录的CSV成功入库时 | `field_name`保存字段名，`unit`保存单位；无单位时为NULL。                                                                                  |
| `L2Cache`               | 对应CSV入库成功                 | [L2Cache](#section-l2cache)                                                                                                               |
| `Memory`                | 对应CSV入库成功                 | [Memory](#section-memory)                                                                                                                 |
| `MemoryL0`              | 对应CSV入库成功                 | [MemoryL0](#section-memoryl0)                                                                                                             |
| `MemoryUB`              | 对应CSV入库成功                 | [MemoryUB](#section-memoryub)                                                                                                             |
| `PcSampling`            | 选择`PmSampling` | 按业务指令地址汇总八种等待或阻塞状态的采样计数，见[PC采样状态说明](appendix/npu_compute_performance_data.md#pcsamplingpc采样状态累计计数)。 |
| `PipeUtilization`       | 对应CSV入库成功                 | [PipeUtilization](#section-pipeutilization)                                                                                               |
| `ResourceConflictRatio` | 对应CSV入库成功                 | [ResourceConflictRatio](#section-resourceconflictratio)                                                                                   |
| `SourceCounter`         | 选择`SourceCounter`时 | 按原始指令地址汇总的执行计数和访存指标，见[SourceCounter指标说明](appendix/npu_compute_performance_data.md#sourcecounter)。 |

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

- 芯片类型：当前仅支持Ascend 950PR&950DT系列产品。
- 编程语言：待采集的算子当前仅支持Ascend C。
- Kernel模式的算子调用方式：当前支持通过`<<<>>>`或aclnn方式进行单算子单次调用。
- `Pipeline`采集受硬件限制：同一Device上只能有一个进程采集`Pipeline`；多个进程同时采集时，只有一个进程能正常输出，其他进程采集失败。
- `PmSampling`采集受硬件限制：同一Device上，`PmSampling`不能与其他进程的`PmSampling`或`Pipeline`采集同时执行；并发采集可能导致采样数据缺失或采集失败。
- `PmSampling`用于分析算子运行时SIMT指令的阻塞情况。硬件按周期采样，当算子中的SIMT代码执行时间过短时，可能因未命中采样时刻而无法获得有效样本，导致`PcSampling`表为空。其他采集项的成功结果仍会保留。
- `SourceCounter`的SIMT UB Bank冲突仅支持SIMT Kernel中线程直接通过`__ubuf__`指针对线程块共享UB数据进行读写的访问。SIMD VF Bank冲突仅支持当前采集模式能够识别的`asc_load`和`asc_store`向量UB读写形式，即从UB读取数据到向量寄存器或从向量寄存器写回UB。
- 采集SIMT算子的`Pipeline`或`PmSampling`数据时，不支持使用`noinline`的`__aicore__`函数，比如`printf`和`asc_dump`。
- 目标程序必须具有执行权限，可以是可执行文件或脚本，能够在未使用`npu-compute`时独立运行。Kernel模式要求运行过程中至少成功执行一次NPU核函数；Application模式无设备采集时保留诊断产物并报告采集不完整。
- 一条采集命令只运行一个目标程序；需要采集多个程序时，分别执行多条命令。目标程序及其启动的脚本不能再次运行`npu-compute`，检测到嵌套采集时本次采集失败且不会生成报告。

## 任务与 block 占用时间线

```bash
mkdir -p occupancy
npu-compute --no-pack -o occupancy ./application
npu-compute --section PipeUtilization -o occupancy.npu-rep ./application
```

`Occupancy.json`采用Chrome Trace的`traceEvents`格式，以完整区间（`ph: X`）展示任务和block，可在Chrome tracing或兼容查看器中打开。默认打包及`--import`均保留该JSON；后续原始launch沿用`collection-p<pid>-<sequence>/Occupancy.json`目录前缀。

每个replay/task/stream使用独立进程轨道，以各自任务起点作为零点，显示单位为微秒。换算频率来自`halGetDeviceInfo`的设备系统计数器查询（`INFO_TYPE_DEV_OSC_FREQUE`），不使用AI Core MHz或`NPU_COMPUTE_FREQUENCY_MHZ`。原始计数器在事件参数中使用十进制字符串，保留大整数精度。

Task window使用TaskLog（`0x00/0x01`）的起止边界。每个block的`blockStartCounter`、`blockEndCounter`及图上区间均使用匹配的128字节block PMU（`0x29`）记录中的起止system counter，`pmuTotalCycles`直接使用同一记录的周期计数。原始BlockLog（`0x24/0x25`）边界另存为`blockLogStartCounter`、`blockLogEndCounter`，`blockCounterSource`标记为`PMU 0x29`。PMU区间可与BlockLog区间不同，不能据此断言区间内每一时刻均在执行指令。

时间线默认生成，复用指标Section已有的PMU replay；仅选择Pipeline、PmSampling或Roofline等不含普通PMU事件的Section时补充一个普通PMU replay用于采集block计数器，不跨replay拼接区间。事件保留launch、device、replay、task、stream、block、subblock及观测到的core身份。每个block使用独立轨道，轨道名为`AIC0`、`AIC1`、…、`AIV0`、`AIV1`、…；区间名按执行类型和原始ID显示，例如`AIC BLOCK2`、`AIV BLOCK2 SUBBLOCK0`；AIC、AIV分别从0连续编号，并在每个replay/task/stream中重新开始。编号仅用于显示，不代表物理core ID；原始blockId、subBlockId、coreId32和coreId128保留在事件参数中。Task window在首行，随后按AIC、AIV及blockId、subBlockId的数字升序排列并分配显示编号；32/128字节记录身份冲突或同一观测core上的PMU区间重叠时输出诊断，保留全部区间。这些标签不推断Cube/Vector物理分组。

系统时钟不可用、replay失败或数据损坏、BlockLog起止记录或匹配的`0x29`记录缺失或重复、时间反向或超出任务窗口、身份歧义、不支持的core类型，以及同一声明core的BlockLog区间重叠会使采集明确失败，不发布空的成功时间线；其他有效产物尽可能保留。

使用`aclptiProfilingDataResult`的ACLPTI SDK调用方须使用更新后的头文件重新编译。Replay快照追加在既有字段之后；range-profiler配置结构体保持不变。
