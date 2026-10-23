# msobjdump

## 概述

本工具主要针对生成的算子ELF文件（Executable and Linkable Format）提供解析、提取和Ascend AICore指令反汇编功能，并将结果信息以可读形式呈现，方便开发者直观获得kernel文件信息。关于本工具的详细介绍请参考《[Ascend C算子开发](https://hiascend.com/document/redirect/CannCommunityOpdevAscendC)》中的“编程指南 > 附录 > msobjdump工具”。

工具调用演示可参考[msobjdump样例](../../examples/04_msobjdump/README.md)。

通过 Agent 使用本工具时，可参考 [tool-npu-objdump Skill](../../skills/tool-npu-objdump/SKILL.md)，了解环境准备、命令选择和元数据解读。

## 命令格式

- 解析ELF文件的命令

    ```bash
    msobjdump --dump-elf ${elf_file} [--verbose]
    ```

    `--dump-elf ${elf_file}`为必选，表示待解析ELF文件路径。[--verbose]为可选，用于开启ELF文件中全量打印device信息功能。

- 解压ELF文件的命令

    ```bash
    msobjdump --extract-elf ${elf_file} [--out-dir ${out_path}]
    ```

    `--extract-elf ${elf_file}`为必选，表示待解析ELF文件路径。[--out-dir ${out_path}]为可选，用于设置解压文件的落盘路径。指定的目录必须已存在且可写，工具不会自动创建；未指定时使用当前工作目录。

- 获取ELF文件列表的命令

    ```bash
    msobjdump --list-elf ${elf_file}
    ```

    `--list-elf ${elf_file}`为必选，获取ELF文件中包含的device信息文件列表，并打印显示。

- 反汇编Ascend AICore指令的命令

    ```bash
    msobjdump --sass ${input_file}
    ```

    `--sass ${input_file}`为必选，用于输出输入文件中Device侧的Ascend AICore指令。该选项支持独立Device ELF、融合编译生成的Host ELF（如可执行文件`demo`或动态库`libcustom.so`）、静态库`libkernels.a`以及包含`.aicore_binary`、`.ascend.kernel.*`或`_o/_json`内容的封装文件。对于Host ELF或静态库，工具会自动遍历并提取其中的Device ELF后进行反汇编；若输入中不包含Device ELF，则返回错误。

    `SASS`仅为命令选项名称，实际输出为Ascend AICore指令，不是NVIDIA SASS。`--sass`不支持与`--dump-elf`、`--extract-elf`、`--list-elf`、`--verbose`或`--out-dir`组合；输出默认写入标准输出，如需保存结果可使用Shell重定向。当前不支持thin archive。

    安装包的主命令为`npu-objdump`，同时提供指向它的`msobjdump`软链接以兼容既有用法，两个命令支持相同的参数和处理逻辑；当前仅提供`--sass`长选项，不提供`-sass`别名。

下表为ELF文件中常见字段说明：

| 字段名 | 含义 | 是否必选 | 打印说明 |
| ---- | ---- | ---- | ---- |
| `.ascend.meta. ${id}` | 表示算子kernel函数名称，其中`${id}`表示meta信息的索引值。 | 是 | 不设置`--verbose`，默认打印。 |
| `VERSION` | 表示版本号。 | 是 | 不设置`--verbose`，默认打印。 |
| `DEBUG` | 调试相关信息，包含如下两部分内容：<br>`debugBufSize`：调试信息需要的内存空间。<br>`debugOptions`：调试开关状态。取值如下：<br>`0`：调试开关关闭。<br>`1`：通过DumpTensor、printf打印进行调试。<br>`2`：通过assert断言进行调试。<br>`4`：通过时间戳打点功能进行调试。<br>`8`：通过内存越界检测进行调试。 | 否 | 不设置`--verbose`，默认打印。 |
| `DYNAMIC_PARAM` | 算子kernel函数是否启用动态参数。取值分别为：<br>`0`：关闭动态参数模式。<br>`1`：开启动态参数模式。 | 否 | 不设置`--verbose`，默认打印。 |
| `OPTIONAL_PARAM` | 可选参数信息，包含如下两部分内容：<br>`optionalInputMode`：可选输入在算子kernel函数中是否需要占位。<br>`0`：可选输入不占位。<br>`1`：可选输入占位。<br>`optionalOutputMode`：可选输出在算子kernel函数中是否需要占位。<br>`0`：可选输出不占位。<br>`1`：可选输出占位。 | 否 | 不设置`--verbose`，默认打印。 |
| `KERNEL_TYPE` | 表示kernel函数运行时core类型。 | 否 | 不设置`--verbose`，默认打印。 |
| `CROSS_CORE_SYNC` | 表示硬同步syncall类型。仅Atlas A3 训练系列产品/Atlas A3 推理系列产品，Atlas A2 训练系列产品/Atlas A2 推理系列产品生效，其他产品上不生效。<br>`USE_SYNC`：使用硬同步。<br>`NO_USE_SYNC`：不使用硬同步。 | 否 | 不设置`--verbose`，默认打印。 |
| `MIX_TASK_RATION` | 表示kernel函数运行时的Cube核/Vector核占比分配类型。 | 否 | 不设置`--verbose`，默认打印。 |
| `DETERMINISTIC_INFO` | 表示算子是否为确定性计算。<br>`0`：不确定计算。<br>`1`：确定性计算。 | 否 | 不设置`--verbose`，默认打印。 |
| `BLOCK_NUM` | 表示算子执行核数，该字段当前暂不支持，只打印默认值`0xFFFFFFFF`。 | 否 | 不设置`--verbose`，默认打印。 |
| `FUNCTION_ENTRY` | 算子TilingKey的值。 | 否 | 不设置`--verbose`，默认打印。 |
| `elf header infos` | 包括ELF Header、Section Headers、Key to Flags、Program Headers、Symbol表等信息。 | 否 | 设置`--verbose`，开启全量打印。 |
