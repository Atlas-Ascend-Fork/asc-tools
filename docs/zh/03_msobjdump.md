# msobjdump

## 概述

本工具主要针对生成的算子ELF文件（Executable and Linkable Format）提供解析、提取和AI Core指令反汇编功能，并将结果信息以可读形式呈现，方便开发者直观获得kernel文件信息。关于本工具的详细介绍请参考《[Ascend C算子开发](https://hiascend.com/document/redirect/CannCommunityOpdevAscendC)》中的“编程指南 > 附录 > msobjdump工具”。

工具调用演示可参考[msobjdump样例](../../examples/04_msobjdump/README.md)。

通过 Agent 使用本工具时，可参考 [tool-npu-objdump Skill](../../skills/tool-npu-objdump/SKILL.md)，了解环境准备、命令选择和元数据解读。

## 命令格式

- 解析ELF文件的命令

    ```bash
    msobjdump --dump-elf ${elf_file} [--verbose]
    ```

    | 参数（区分大小写） | 可选/必选 | 说明 |
    | ---- | ---- | ---- |
    | `--dump-elf ${elf_file}`，`-d` | 必选 | 解析ELF文件中包含的device信息，并打印显示。<br>`${elf_file}`表示待解析的ELF文件路径。 |
    | `--verbose`，`-V` | 可选 | 与`--dump-elf`配套使用，开启ELF文件中全量打印device信息功能。 |

- 解压ELF文件的命令

    ```bash
    msobjdump --extract-elf ${elf_file} [--out-dir ${out_path}]
    ```

    | 参数（区分大小写） | 可选/必选 | 说明 |
    | ---- | ---- | ---- |
    | `--extract-elf ${elf_file}`，`-e` | 必选 | 解压ELF文件中包含的device信息，并将结果保存到输出路径下。<br>`${elf_file}`表示待解压的ELF文件路径，默认输出路径为当前执行路径。 |
    | `--out-dir ${out_path}`，`-o` | 可选 | 与`--extract-elf`配套使用，设置解压文件的落盘路径。指定的目录必须已存在且可写，工具不会自动创建；未指定时使用当前工作目录。 |

- 获取ELF文件列表的命令

    ```bash
    msobjdump --list-elf ${elf_file}
    ```

    | 参数（区分大小写） | 可选/必选 | 说明 |
    | ---- | ---- | ---- |
    | `--list-elf ${elf_file}`，`-l` | 必选 | 获取ELF文件中包含的device信息文件列表，并打印显示。<br>`${elf_file}`表示待打印的ELF文件路径。 |

- 反汇编AI Core指令的命令

    ```bash
    msobjdump --sass ${input_file} [--lineinfo] [--source]
    ```

    | 参数（区分大小写） | 可选/必选 | 说明 |
    | ---- | ---- | ---- |
    | `--sass ${input_file}` | 必选 | 反汇编输入的ELF文件，显示Device侧的AI Core指令；`${input_file}`表示待反汇编的ELF文件路径。 |
    | `--lineinfo` | 可选 | 与`--sass`配套使用，在反汇编结果中显示源文件名和行号。编译算子时需添加`-g`生成调试信息。 |
    | `--source`，`-s` | 可选 | 与`--sass`配套使用，在反汇编结果中显示对应源码。编译算子时需添加`-g`生成调试信息，并确保调试信息记录路径下的源文件可访问。 |

    支持独立Device ELF、融合编译生成的Host ELF（如可执行文件`demo`或动态库`libcustom.so`）、静态库`libkernels.a`以及包含`.aicore_binary`、`.ascend.kernel.*`或`_o/_json`内容的封装文件。对于Host ELF或静态库，工具会自动遍历并提取其中的Device ELF后进行反汇编；若输入中不包含Device ELF，则返回错误。

    `--sass`不支持与`--dump-elf`、`--extract-elf`、`--list-elf`、`--verbose`或`--out-dir`组合使用；输出默认写入标准输出，如需保存结果可使用Shell重定向。当前不支持thin archive。

    安装包的主命令为`npu-objdump`，同时提供指向它的`msobjdump`软链接以兼容既有用法，两个命令支持相同的参数和处理逻辑；当前仅提供`--sass`长选项，不提供`-sass`别名。

    `--lineinfo` 和 `--source` 依赖 Device ELF 中的调试信息。编译算子时请使用 `-g` 生成调试信息，配置方法请参见[常用的编译选项](https://gitcode.com/cann/asc-devkit/blob/master/docs/zh/guide/programming_guide/compilation_and_execution/operator_compilation/ai_core_operator_compilation.md#ZH-CN_TOPIC_0000002462746461)和[自定义编译选项](https://gitcode.com/cann/asc-devkit/blob/master/docs/zh/guide/programming_guide/advanced_programming/aclnn_operator_development/compilation_and_deployment/basic_process.md#custom-compile-options)。即使ELF不含调试信息，仍可使用`--sass`输出反汇编指令；此时`--lineinfo`无法显示源码位置，`--source`无法显示源码。使用`--source`时，若调试信息记录的源文件无法访问，工具会输出告警并继续反汇编，但不显示对应源码。


下表为ELF文件中常见字段说明：

**表1**  核函数（Kernel）直调工程支持的ELF解析字段说明<a name="table94384560259"></a>

| 字段名 | 含义 | 是否必选 | 打印说明 |
| ---- | ---- | ---- | ---- |
| `.ascend.meta. ${id}` | 表示算子核函数（Kernel）名称，其中`${id}`表示meta信息的索引值。 | 是 | 不设置`--verbose`，默认打印。 |
| `VERSION` | 表示版本号。 | 是 | 不设置`--verbose`，默认打印。 |
| `RUNTIME_IMPLICIT_INFO` | 表示运行时隐式信息标志。取值如下：<br>`1`：`SIMD Printf Flag`，表示SIMD侧Printf标志。<br>`2`：`Hardware Sync Flag`，表示硬同步标志。<br>`3`：`L2Cache Hint Flag`，表示L2 Cache命中标志。<br>`4`：`SIMT Printf Flag`，表示SIMT侧Printf标志。<br>`5`：`SIMD Assert Flag`，表示SIMD侧Assert标志。<br>其他取值打印原始数值。 | 否 | 不设置`--verbose`，默认打印。 |
| `KERNEL_TYPE` | 表示核函数（Kernel）运行时core类型，取值参见<a href="#table187419221164">表3</a>。 | 否 | 不设置`--verbose`，默认打印。 |
| `CROSS_CORE_SYNC` | 表示硬同步syncall类型。<br>`USE_SYNC`：使用硬同步。<br>`NO_USE_SYNC`：不使用硬同步。<br><!-- npu="950" id1 -->Ascend 950PR&950DT系列产品：不支持<br><!-- end id1 --><!-- npu="A3" id2 -->Atlas A3系列产品：支持<br><!-- end id2 --><!-- npu="910b" id3 -->Atlas A2系列产品：支持<br><!-- end id3 --><!-- npu="310b" id4 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id4 --><!-- npu="310p" id5 -->Atlas推理系列产品：不支持<br><!-- end id5 --><!-- npu="910" id6 -->Atlas训练系列产品：不支持<!-- end id6 --> | 否 | 不设置`--verbose`，默认打印。 |
| `MIX_TASK_RATION` | 表示核函数（Kernel）运行时的Cube核/Vector核占比分配类型。 | 否 | 不设置`--verbose`，默认打印。 |
| `SK_INFO` | SuperKernel的核函数与子函数绑定信息，包含以下字段：<br>`reserved`：保留字段。<br>`cap`：SuperKernel特性位图，各位含义参见[SK_BIND参数说明](https://gitcode.com/cann/asc-devkit/blob/master/docs/zh/api/Utils-API/SuperKernel/SK_BIND.md)。<br>`global`：Kernel函数入口地址。<br>`sk0`～`sk3`：绑定的SK子函数入口地址。<br><!-- npu="950" id7 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id7 --><!-- npu="A3" id8 -->Atlas A3系列产品：支持<br><!-- end id8 --><!-- npu="910b" id9 -->Atlas A2系列产品：支持<br><!-- end id9 --><!-- npu="310b" id10 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id10 --><!-- npu="310p" id11 -->Atlas推理系列产品：不支持<br><!-- end id11 --><!-- npu="910" id12 -->Atlas训练系列产品：不支持<!-- end id12 --> | 否 | 不设置`--verbose`，默认打印。 |
| `ICACHE_PRELOAD` | 指令缓存预加载信息。<br>`flag`：`0`表示不预加载，`1`表示预加载。<br>`reserved`：保留字段。<br><!-- npu="950" id13 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id13 --><!-- npu="A3" id14 -->Atlas A3系列产品：支持<br><!-- end id14 --><!-- npu="910b" id15 -->Atlas A2系列产品：支持<br><!-- end id15 --><!-- npu="310b" id16 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id16 --><!-- npu="310p" id17 -->Atlas推理系列产品：不支持<br><!-- end id17 --><!-- npu="910" id18 -->Atlas训练系列产品：不支持<!-- end id18 --> | 否 | 不设置`--verbose`，默认打印。 |
| `COMPILER_ALLOC_UB_SIZE` | 编译器分配的UB大小。<br><!-- npu="950" id19 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id19 --><!-- npu="A3" id20 -->Atlas A3系列产品：不支持<br><!-- end id20 --><!-- npu="910b" id21 -->Atlas A2系列产品：不支持<br><!-- end id21 --><!-- npu="310b" id22 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id22 --><!-- npu="310p" id23 -->Atlas推理系列产品：不支持<br><!-- end id23 --><!-- npu="910" id24 -->Atlas训练系列产品：不支持<!-- end id24 --> | 否 | 不设置`--verbose`，默认打印。 |
| `SU_STACK_SIZE` | 核函数所需的Scalar Unit（SU）栈大小。<br><!-- npu="950" id25 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id25 --><!-- npu="A3" id26 -->Atlas A3系列产品：不支持<br><!-- end id26 --><!-- npu="910b" id27 -->Atlas A2系列产品：不支持<br><!-- end id27 --><!-- npu="310b" id28 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id28 --><!-- npu="310p" id29 -->Atlas推理系列产品：不支持<br><!-- end id29 --><!-- npu="910" id30 -->Atlas训练系列产品：不支持<!-- end id30 --> | 否 | 不设置`--verbose`，默认打印。 |
| `SIMT_WARP_STACK_SIZE` | SIMT通用栈大小。<br><!-- npu="950" id31 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id31 --><!-- npu="A3" id32 -->Atlas A3系列产品：不支持<br><!-- end id32 --><!-- npu="910b" id33 -->Atlas A2系列产品：不支持<br><!-- end id33 --><!-- npu="310b" id34 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id34 --><!-- npu="310p" id35 -->Atlas推理系列产品：不支持<br><!-- end id35 --><!-- npu="910" id36 -->Atlas训练系列产品：不支持<!-- end id36 --> | 否 | 不设置`--verbose`，默认打印。 |
| `SIMT_DVG_WARP_STACK_SIZE` | SIMT Divergence（分支）栈大小。<br><!-- npu="950" id37 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id37 --><!-- npu="A3" id38 -->Atlas A3系列产品：不支持<br><!-- end id38 --><!-- npu="910b" id39 -->Atlas A2系列产品：不支持<br><!-- end id39 --><!-- npu="310b" id40 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id40 --><!-- npu="310p" id41 -->Atlas推理系列产品：不支持<br><!-- end id41 --><!-- npu="910" id42 -->Atlas训练系列产品：不支持<!-- end id42 --> | 否 | 不设置`--verbose`，默认打印。 |
| `AIV_TYPE_FLAG` | AIV执行类型。SU（Scalar Unit），负责标量运算及向其他计算单元发射指令。<br>`1`：Scalar Unit（SU）。<br>`2`：Scalar Unit（SU）+ SIMD。<br>`3`：Scalar Unit（SU）+ SIMT。<br>`4`：Scalar Unit（SU）+ SIMD + SIMT。<br><!-- npu="950" id43 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id43 --><!-- npu="A3" id44 -->Atlas A3系列产品：不支持<br><!-- end id44 --><!-- npu="910b" id45 -->Atlas A2系列产品：不支持<br><!-- end id45 --><!-- npu="310b" id46 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id46 --><!-- npu="310p" id47 -->Atlas推理系列产品：不支持<br><!-- end id47 --><!-- npu="910" id48 -->Atlas训练系列产品：不支持<!-- end id48 --> | 否 | 不设置`--verbose`，默认打印。 |
| `PARAM_SUMMARY` | 参数汇总。<br>`paraNums`：参数数量。<br>`paramTotalSize`：参数区总大小。 与`PARAM_INFO`配合描述参数布局。<br><!-- npu="950" id49 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id49 --><!-- npu="A3" id50 -->Atlas A3系列产品：支持<br><!-- end id50 --><!-- npu="910b" id51 -->Atlas A2系列产品：支持<br><!-- end id51 --><!-- npu="310b" id52 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id52 --><!-- npu="310p" id53 -->Atlas推理系列产品：不支持<br><!-- end id53 --><!-- npu="910" id54 -->Atlas训练系列产品：不支持<!-- end id54 --> | 否 | 不设置`--verbose`，默认打印。 |
| `PARAM_INFO` | 单个参数的布局。<br>`ordinal`：参数序号，从0开始。<br>`offset`：参数在参数区内的偏移。<br>`size`：参数槽位大小。<br>`align`：参数槽位对齐要求。<br>`paramType`取值：`1`为标量，`2`为指针。<br>`index`、`spaceIndex`、`spaceType`、`space`、`resv`：保留字段，当前未使用。<br><!-- npu="950" id55 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id55 --><!-- npu="A3" id56 -->Atlas A3系列产品：支持<br><!-- end id56 --><!-- npu="910b" id57 -->Atlas A2系列产品：支持<br><!-- end id57 --><!-- npu="310b" id58 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id58 --><!-- npu="310p" id59 -->Atlas推理系列产品：不支持<br><!-- end id59 --><!-- npu="910" id60 -->Atlas训练系列产品：不支持<!-- end id60 --> | 否 | 不设置`--verbose`，默认打印。 |
| `SCHED_MODE` | 核函数调度模式。`0`为普通模式，`1`为Batch模式。<br><!-- npu="950" id61 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id61 --><!-- npu="A3" id62 -->Atlas A3系列产品：支持<br><!-- end id62 --><!-- npu="910b" id63 -->Atlas A2系列产品：支持<br><!-- end id63 --><!-- npu="310b" id64 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id64 --><!-- npu="310p" id65 -->Atlas推理系列产品：不支持<br><!-- end id65 --><!-- npu="910" id66 -->Atlas训练系列产品：不支持<!-- end id66 --> | 否 | 不设置`--verbose`，默认打印。 |
| `DEVICE_FUNC_TYPE` | 非Kernel函数类型。<br>`1`：融合架构主标量函数（MainScalar）。<br>`2`：分离架构Cube侧主标量函数。<br>`3`：分离架构Vector侧主标量函数。<br>`4`：SIMD函数。<br>`5`：SIMT入口函数。<br>`6`：SIMT子函数。<br>当前未启用。 | 否 | 不设置`--verbose`，默认打印。 |
| `SIMT_LAUNCH_BOUNDS` | SIMT最大线程数。<br><!-- npu="950" id67 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id67 --><!-- npu="A3" id68 -->Atlas A3系列产品：不支持<br><!-- end id68 --><!-- npu="910b" id69 -->Atlas A2系列产品：不支持<br><!-- end id69 --><!-- npu="310b" id70 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id70 --><!-- npu="310p" id71 -->Atlas推理系列产品：不支持<br><!-- end id71 --><!-- npu="910" id72 -->Atlas训练系列产品：不支持<!-- end id72 --> | 否 | 不设置`--verbose`，默认打印。 |
| `elf header infos` | 包括ELF Header、Section Headers、Key to Flags、Program Headers、Symbol表等信息。 | 否 | 设置`--verbose`，开启全量打印。 |

**表2**  自定义算子工程支持的ELF解析字段说明<a name="table-aclnn-meta-fields"></a>

| 字段名 | 含义 | 是否必选 | 打印说明 |
| ---- | ---- | ---- | ---- |
| `.ascend.meta. ${id}` | 表示算子核函数（Kernel）名称，其中`${id}`表示meta信息的索引值。 | 是 | 不设置`--verbose`，默认打印。 |
| `VERSION` | 表示版本号。<br><!-- npu="950" id73 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id73 --><!-- npu="A3" id74 -->Atlas A3系列产品：不支持<br><!-- end id74 --><!-- npu="910b" id75 -->Atlas A2系列产品：不支持<br><!-- end id75 --><!-- npu="310b" id76 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id76 --><!-- npu="310p" id77 -->Atlas推理系列产品：不支持<br><!-- end id77 --><!-- npu="910" id78 -->Atlas训练系列产品：不支持<!-- end id78 --> | 否 | 不设置`--verbose`，默认打印。 |
| `DEBUG` | 调试相关信息，包含如下两部分内容：<br>`debugBufSize`：调试信息需要的内存空间。<br>`debugOptions`：调试开关状态。取值如下：<br>`0`：调试开关关闭。<br>`1`：通过DumpTensor、printf打印进行调试。<br>`2`：通过assert断言进行调试。<br>`4`：通过时间戳打点功能进行调试。<br>`8`：通过内存越界检测进行调试。<br><!-- npu="950" id79 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id79 --><!-- npu="A3" id80 -->Atlas A3系列产品：不支持<br><!-- end id80 --><!-- npu="910b" id81 -->Atlas A2系列产品：不支持<br><!-- end id81 --><!-- npu="310b" id82 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id82 --><!-- npu="310p" id83 -->Atlas推理系列产品：不支持<br><!-- end id83 --><!-- npu="910" id84 -->Atlas训练系列产品：不支持<!-- end id84 --> | 否 | 不设置`--verbose`，默认打印。 |
| `DYNAMIC_PARAM` | 算子核函数（Kernel）是否启用动态参数。取值分别为：<br>`0`：关闭动态参数模式。<br>`1`：开启动态参数模式。<br><!-- npu="950" id85 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id85 --><!-- npu="A3" id86 -->Atlas A3系列产品：不支持<br><!-- end id86 --><!-- npu="910b" id87 -->Atlas A2系列产品：不支持<br><!-- end id87 --><!-- npu="310b" id88 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id88 --><!-- npu="310p" id89 -->Atlas推理系列产品：不支持<br><!-- end id89 --><!-- npu="910" id90 -->Atlas训练系列产品：不支持<!-- end id90 --> | 否 | 不设置`--verbose`，默认打印。 |
| `OPTIONAL_PARAM` | 可选参数信息，包含如下两部分内容：<br>`optionalInputMode`：可选输入在算子核函数（Kernel）中是否需要占位。<br>`0`：可选输入不占位。<br>`1`：可选输入占位。<br>`optionalOutputMode`：可选输出在算子核函数（Kernel）中是否需要占位。<br>`0`：可选输出不占位。<br>`1`：可选输出占位。<br><!-- npu="950" id91 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id91 --><!-- npu="A3" id92 -->Atlas A3系列产品：不支持<br><!-- end id92 --><!-- npu="910b" id93 -->Atlas A2系列产品：不支持<br><!-- end id93 --><!-- npu="310b" id94 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id94 --><!-- npu="310p" id95 -->Atlas推理系列产品：不支持<br><!-- end id95 --><!-- npu="910" id96 -->Atlas训练系列产品：不支持<!-- end id96 --> | 否 | 不设置`--verbose`，默认打印。 |
| `RUNTIME_IMPLICIT_INFO` | 表示运行时隐式信息标志。取值如下：<br>`1`：`SIMD Printf Flag`，表示SIMD侧Printf标志。<br>`2`：`Hardware Sync Flag`，表示硬同步标志。<br>`3`：`L2Cache Hint Flag`，表示L2 Cache命中标志。<br>`4`：`SIMT Printf Flag`，表示SIMT侧Printf标志。<br>`5`：`SIMD Assert Flag`，表示SIMD侧Assert标志。<br>其他取值打印原始数值。 | 否 | 不设置`--verbose`，默认打印。 |
| `KERNEL_TYPE` | 表示核函数（Kernel）运行时core类型，取值参见<a href="#table187419221164">表3</a>。 | 否 | 不设置`--verbose`，默认打印。 |
| `CROSS_CORE_SYNC` | 表示硬同步syncall类型。<br>`USE_SYNC`：使用硬同步。<br>`NO_USE_SYNC`：不使用硬同步。<br><!-- npu="950" id97 -->Ascend 950PR&950DT系列产品：不支持<br><!-- end id97 --><!-- npu="A3" id98 -->Atlas A3系列产品：支持<br><!-- end id98 --><!-- npu="910b" id99 -->Atlas A2系列产品：支持<br><!-- end id99 --><!-- npu="310b" id100 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id100 --><!-- npu="310p" id101 -->Atlas推理系列产品：不支持<br><!-- end id101 --><!-- npu="910" id102 -->Atlas训练系列产品：不支持<!-- end id102 --> | 否 | 不设置`--verbose`，默认打印。 |
| `MIX_TASK_RATION` | 表示核函数（Kernel）运行时的Cube核/Vector核占比分配类型。 | 否 | 不设置`--verbose`，默认打印。 |
| `DETERMINISTIC_INFO` | 表示算子是否为确定性计算。<br>`0`：不确定计算。<br>`1`：确定性计算。<br><!-- npu="950" id103 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id103 --><!-- npu="A3" id104 -->Atlas A3系列产品：不支持<br><!-- end id104 --><!-- npu="910b" id105 -->Atlas A2系列产品：不支持<br><!-- end id105 --><!-- npu="310b" id106 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id106 --><!-- npu="310p" id107 -->Atlas推理系列产品：不支持<br><!-- end id107 --><!-- npu="910" id108 -->Atlas训练系列产品：不支持<!-- end id108 --> | 否 | 不设置`--verbose`，默认打印。 |
| `BLOCK_NUM` | 表示算子执行核数，`0xFFFFFFFF`为默认值，当前未启用。<br><!-- npu="950" id109 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id109 --><!-- npu="A3" id110 -->Atlas A3系列产品：不支持<br><!-- end id110 --><!-- npu="910b" id111 -->Atlas A2系列产品：不支持<br><!-- end id111 --><!-- npu="310b" id112 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id112 --><!-- npu="310p" id113 -->Atlas推理系列产品：不支持<br><!-- end id113 --><!-- npu="910" id114 -->Atlas训练系列产品：不支持<!-- end id114 --> | 否 | 不设置`--verbose`，默认打印。 |
| `FUNCTION_ENTRY` | 算子TilingKey的值。<br><!-- npu="950" id115 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id115 --><!-- npu="A3" id116 -->Atlas A3系列产品：不支持<br><!-- end id116 --><!-- npu="910b" id117 -->Atlas A2系列产品：不支持<br><!-- end id117 --><!-- npu="310b" id118 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id118 --><!-- npu="310p" id119 -->Atlas推理系列产品：不支持<br><!-- end id119 --><!-- npu="910" id120 -->Atlas训练系列产品：不支持<!-- end id120 --> | 否 | 不设置`--verbose`，默认打印。 |
| `SK_INFO` | SuperKernel的核函数与子函数绑定信息，包含以下字段：<br>`reserved`：保留字段。<br>`cap`：SuperKernel特性位图，各位含义参见[SK_BIND参数说明](https://gitcode.com/cann/asc-devkit/blob/master/docs/zh/api/Utils-API/SuperKernel/SK_BIND.md)。<br>`global`：Kernel函数入口地址。<br>`sk0`～`sk3`：绑定的SK子函数入口地址。<br><!-- npu="950" id121 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id121 --><!-- npu="A3" id122 -->Atlas A3系列产品：支持<br><!-- end id122 --><!-- npu="910b" id123 -->Atlas A2系列产品：支持<br><!-- end id123 --><!-- npu="310b" id124 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id124 --><!-- npu="310p" id125 -->Atlas推理系列产品：不支持<br><!-- end id125 --><!-- npu="910" id126 -->Atlas训练系列产品：不支持<!-- end id126 --> | 否 | 不设置`--verbose`，默认打印。 |
| `SPECIALIZATION_RESOURCE_ID` | 算子二进制资源标识，显示为64个字符的SHA-256摘要。<br><!-- npu="950" id127 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id127 --><!-- npu="A3" id128 -->Atlas A3系列产品：支持<br><!-- end id128 --><!-- npu="910b" id129 -->Atlas A2系列产品：支持<br><!-- end id129 --><!-- npu="310b" id130 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id130 --><!-- npu="310p" id131 -->Atlas推理系列产品：不支持<br><!-- end id131 --><!-- npu="910" id132 -->Atlas训练系列产品：不支持<!-- end id132 --> | 否 | 不设置`--verbose`，默认打印。 |
| `ICACHE_PRELOAD` | 指令缓存预加载信息。<br>`flag`：`0`表示不预加载，`1`表示预加载。<br>`reserved`：保留字段。<br><!-- npu="950" id133 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id133 --><!-- npu="A3" id134 -->Atlas A3系列产品：支持<br><!-- end id134 --><!-- npu="910b" id135 -->Atlas A2系列产品：支持<br><!-- end id135 --><!-- npu="310b" id136 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id136 --><!-- npu="310p" id137 -->Atlas推理系列产品：不支持<br><!-- end id137 --><!-- npu="910" id138 -->Atlas训练系列产品：不支持<!-- end id138 --> | 否 | 不设置`--verbose`，默认打印。 |
| `SPECIALIZATION_FLAG` | 二进制算子常量化状态。<br>`0`：静态。<br>`1`：动态。<br><!-- npu="950" id139 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id139 --><!-- npu="A3" id140 -->Atlas A3系列产品：支持<br><!-- end id140 --><!-- npu="910b" id141 -->Atlas A2系列产品：支持<br><!-- end id141 --><!-- npu="310b" id142 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id142 --><!-- npu="310p" id143 -->Atlas推理系列产品：不支持<br><!-- end id143 --><!-- npu="910" id144 -->Atlas训练系列产品：不支持<!-- end id144 --> | 否 | 不设置`--verbose`，默认打印。 |
| `DFX_INFO` | 异常调测时使用的参数信息，由多个`DFX_ARG_INFO`记录组成，按参数展开打印。<br><!-- npu="950" id145 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id145 --><!-- npu="A3" id146 -->Atlas A3系列产品：支持<br><!-- end id146 --><!-- npu="910b" id147 -->Atlas A2系列产品：支持<br><!-- end id147 --><!-- npu="310b" id148 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id148 --><!-- npu="310p" id149 -->Atlas推理系列产品：不支持<br><!-- end id149 --><!-- npu="910" id150 -->Atlas训练系列产品：不支持<!-- end id150 --> | 否 | 不设置`--verbose`，默认打印。 |
| `DFX_ARG_INFO` | 单个参数的异常调测信息。<br>`args_type`：参数类别和指针层级的编码值。<br>`param_type`：参数类别。<br>`1`：Tensor。<br>`2`：输入。<br>`3`：输出。<br>`4`：工作空间。<br>`5`：Ascend C日志参数。<br>`6`：MC2通信上下文。<br>`7`：Tiling。<br>`8`：L1。<br>`9`：L2。<br>`10`：溢出检测参数。<br>`11`：FFTS硬同步参数。<br>`12`：形状Tensor。<br>`point_type`：指针层级及形状信息。<br>`1`：一级指针。<br>`2`：二级指针。<br>`3`：携带形状信息的二级指针。<br>`4`：形状Tensor的一级指针。<br>`param_size`：参数所指数据大小。<br>`param_dtype`：单个元素的存储宽度。<br>`param_dim`：维数。<br>`param_shape`：各维长度。<br>根据记录长度打印对应字段；字段值为`-1`时表示未指定。<br><!-- npu="950" id151 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id151 --><!-- npu="A3" id152 -->Atlas A3系列产品：支持<br><!-- end id152 --><!-- npu="910b" id153 -->Atlas A2系列产品：支持<br><!-- end id153 --><!-- npu="310b" id154 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id154 --><!-- npu="310p" id155 -->Atlas推理系列产品：不支持<br><!-- end id155 --><!-- npu="910" id156 -->Atlas训练系列产品：不支持<!-- end id156 --> | 否 | 不设置`--verbose`，默认打印。 |
| `L0_EXCEPTION_DFX_IS_TIK` | 是否为TIK算子的标志。<br>`0`：非TIK。<br>`1`：TIK。<br><!-- npu="950" id157 -->Ascend 950PR&950DT系列产品：不支持<br><!-- end id157 --><!-- npu="A3" id158 -->Atlas A3系列产品：支持<br><!-- end id158 --><!-- npu="910b" id159 -->Atlas A2系列产品：支持<br><!-- end id159 --><!-- npu="310b" id160 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id160 --><!-- npu="310p" id161 -->Atlas推理系列产品：不支持<br><!-- end id161 --><!-- npu="910" id162 -->Atlas训练系列产品：不支持<!-- end id162 --> | 否 | 不设置`--verbose`，默认打印。 |
| `COMPILER_ALLOC_UB_SIZE` | 编译器分配的UB大小。<br><!-- npu="950" id163 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id163 --><!-- npu="A3" id164 -->Atlas A3系列产品：不支持<br><!-- end id164 --><!-- npu="910b" id165 -->Atlas A2系列产品：不支持<br><!-- end id165 --><!-- npu="310b" id166 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id166 --><!-- npu="310p" id167 -->Atlas推理系列产品：不支持<br><!-- end id167 --><!-- npu="910" id168 -->Atlas训练系列产品：不支持<!-- end id168 --> | 否 | 不设置`--verbose`，默认打印。 |
| `SU_STACK_SIZE` | 核函数所需的Scalar Unit（SU）栈大小。<br><!-- npu="950" id169 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id169 --><!-- npu="A3" id170 -->Atlas A3系列产品：不支持<br><!-- end id170 --><!-- npu="910b" id171 -->Atlas A2系列产品：不支持<br><!-- end id171 --><!-- npu="310b" id172 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id172 --><!-- npu="310p" id173 -->Atlas推理系列产品：不支持<br><!-- end id173 --><!-- npu="910" id174 -->Atlas训练系列产品：不支持<!-- end id174 --> | 否 | 不设置`--verbose`，默认打印。 |
| `SIMT_WARP_STACK_SIZE` | SIMT通用栈大小。<br><!-- npu="950" id175 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id175 --><!-- npu="A3" id176 -->Atlas A3系列产品：不支持<br><!-- end id176 --><!-- npu="910b" id177 -->Atlas A2系列产品：不支持<br><!-- end id177 --><!-- npu="310b" id178 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id178 --><!-- npu="310p" id179 -->Atlas推理系列产品：不支持<br><!-- end id179 --><!-- npu="910" id180 -->Atlas训练系列产品：不支持<!-- end id180 --> | 否 | 不设置`--verbose`，默认打印。 |
| `SIMT_DVG_WARP_STACK_SIZE` | SIMT Divergence（分支）栈大小。<br><!-- npu="950" id181 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id181 --><!-- npu="A3" id182 -->Atlas A3系列产品：不支持<br><!-- end id182 --><!-- npu="910b" id183 -->Atlas A2系列产品：不支持<br><!-- end id183 --><!-- npu="310b" id184 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id184 --><!-- npu="310p" id185 -->Atlas推理系列产品：不支持<br><!-- end id185 --><!-- npu="910" id186 -->Atlas训练系列产品：不支持<!-- end id186 --> | 否 | 不设置`--verbose`，默认打印。 |
| `AIV_TYPE_FLAG` | AIV执行类型。SU（Scalar Unit），负责标量运算及向其他计算单元发射指令。<br>`1`：Scalar Unit（SU）。<br>`2`：Scalar Unit（SU）+ SIMD。<br>`3`：Scalar Unit（SU）+ SIMT。<br>`4`：Scalar Unit（SU）+ SIMD + SIMT。<br><!-- npu="950" id187 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id187 --><!-- npu="A3" id188 -->Atlas A3系列产品：不支持<br><!-- end id188 --><!-- npu="910b" id189 -->Atlas A2系列产品：不支持<br><!-- end id189 --><!-- npu="310b" id190 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id190 --><!-- npu="310p" id191 -->Atlas推理系列产品：不支持<br><!-- end id191 --><!-- npu="910" id192 -->Atlas训练系列产品：不支持<!-- end id192 --> | 否 | 不设置`--verbose`，默认打印。 |
| `PARAM_SUMMARY` | 参数汇总。<br>`paraNums`：参数数量。<br>`paramTotalSize`：参数区总大小。 与`PARAM_INFO`配合描述参数布局。<br><!-- npu="950" id193 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id193 --><!-- npu="A3" id194 -->Atlas A3系列产品：支持<br><!-- end id194 --><!-- npu="910b" id195 -->Atlas A2系列产品：支持<br><!-- end id195 --><!-- npu="310b" id196 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id196 --><!-- npu="310p" id197 -->Atlas推理系列产品：不支持<br><!-- end id197 --><!-- npu="910" id198 -->Atlas训练系列产品：不支持<!-- end id198 --> | 否 | 不设置`--verbose`，默认打印。 |
| `PARAM_INFO` | 单个参数的布局。<br>`ordinal`：参数序号，从0开始。<br>`offset`：参数在参数区内的偏移。<br>`size`：参数槽位大小。<br>`align`：参数槽位对齐要求。<br>`paramType`取值：`1`为标量，`2`为指针。<br>`index`、`spaceIndex`、`spaceType`、`space`、`resv`：保留字段，当前未使用。<br><!-- npu="950" id199 -->Ascend 950PR&950DT系列产品：支持<br><!-- end id199 --><!-- npu="A3" id200 -->Atlas A3系列产品：支持<br><!-- end id200 --><!-- npu="910b" id201 -->Atlas A2系列产品：支持<br><!-- end id201 --><!-- npu="310b" id202 -->Atlas 200I/500 A2推理产品：不支持<br><!-- end id202 --><!-- npu="310p" id203 -->Atlas推理系列产品：不支持<br><!-- end id203 --><!-- npu="910" id204 -->Atlas训练系列产品：不支持<!-- end id204 --> | 否 | 不设置`--verbose`，默认打印。 |
| `DEVICE_FUNC_TYPE` | 非Kernel函数类型。<br>`1`：融合架构主标量函数（MainScalar）。<br>`2`：分离架构Cube侧主标量函数。<br>`3`：分离架构Vector侧主标量函数。<br>`4`：SIMD函数。<br>`5`：SIMT入口函数。<br>`6`：SIMT子函数。<br>当前未启用。 | 否 | 不设置`--verbose`，默认打印。 |
| `elf header infos` | 包括ELF Header、Section Headers、Key to Flags、Program Headers、Symbol表等信息。 | 否 | 设置`--verbose`，开启全量打印。 |

**表3**  核函数（Kernel）类型信息<a name="table187419221164"></a>

| KERNEL_TYPE | 说明 |
| ---- | ---- |
| `AICORE` | **该参数为预留参数，当前版本暂不支持。**<br>算子执行时仅会启动AI Core，比如用户在host侧设置blocknum为5，则会启动5个AI Core。 |
| `AIC` | 算子执行时仅启动AI Core上的Cube核，比如用户在host侧设置blocknum为10，则会启动10个Cube核。 |
| `AIV` | 算子执行时仅启动AI Core上的Vector核，比如用户在host侧设置blocknum为10，则会启动10个Vector核。 |
| `MIX_AIC_MAIN` | AIC、AIV混合场景下，设置核函数（Kernel）的类型为MIX，算子执行时会同时启动AI Core上的Cube核和Vector核，比如用户在host侧设置blocknum为10，且设置task_ration为1：2，则会启动10个Cube核和20个Vector核。 |
| `MIX_AIV_MAIN` | AIC、AIV混合场景下，使用了多核控制相关指令时，设置核函数（Kernel）的类型为MIX，算子执行时会同时启动AI Core上的Cube核和Vector核，比如用户在host侧设置blocknum为10，且设置task_ration为1：2，则会启动10个Vector核和20个Cube核。 |
| `AIC_ROLLBACK` | 算子执行时会同时启动AI Core和Vector Core，此时AI Core会当成Cube Core使用。 |
| `AIV_ROLLBACK` | 算子执行时会同时启动AI Core和Vector Core，此时AI Core会当成Vector Core使用。 |
