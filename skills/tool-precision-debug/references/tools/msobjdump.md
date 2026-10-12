# msobjdump：查看、提取与反汇编 Ascend C 产物

`msobjdump` 读取 Ascend C 编译产物，查看 Kernel 元数据、列出或提取内嵌设备 ELF，并在工具支持时反汇编 Ascend AICore 指令。它静态读取文件，不执行输入程序；分析已有 ELF 不需要先运行算子。

## 使用场景

| 当前需要确认什么 | 使用方式 |
| --- | --- |
| 产物中有哪些 Kernel、核类型或 TilingKey | `--dump-elf` 查看元数据。 |
| ELF 的段、符号及文件头 | 在 `--dump-elf` 后加 `--verbose`，查看当前格式支持的详细输出。 |
| 封装产物中包含哪些设备文件 | `--list-elf` 列出内嵌文件。 |
| 单独检查封装中的某个 Kernel 文件 | `--extract-elf` 提取，再对实际生成的设备 ELF 查看元数据。 |
| 编译后生成了哪些设备指令 | 帮助中提供 `--sass` 时，用它反汇编设备代码。 |

输入可以是独立设备 ELF，也可以是含设备代码的融合编译可执行文件、动态库或其他封装。文件后缀不能说明内容；普通 Host ELF 即使扩展名同样是 `.o` 或 `.so`，也不一定包含 Ascend Kernel。

## 调用准备

使用工程实际采用的 CANN 环境，先确认安装版能力：

```bash
msobjdump -h
```

查看元数据依赖 `readelf`；处理 `.aicore_binary` 融合产物还需要 `llvm-objcopy`，处理静态库可能用到 `ar`。`--sass` 需要带 Ascend 反汇编后端的 `llvm-objdump`，系统通用 LLVM 工具不一定具备该能力。

以下 `<artifact-dir>` 表示本次任务产物目录，使用前替换为实际路径。在独立可写工作目录运行命令；查看、列出操作也可能创建临时文件，部分静态库处理会将成员写到当前目录。每次提取使用单独的输出目录，避免覆盖已有同名文件。

## 常用命令

### 查看 Kernel 元数据与详细结构

```bash
msobjdump --dump-elf "<input-elf>"
msobjdump --dump-elf "<input-elf>" --verbose
```

默认输出以 Kernel 元数据为主；`--verbose` 请求额外的 ELF 结构信息，不是反汇编。检查融合产物时，详细输出可能来自提取后的设备 ELF；不要把其中的 Machine、段或符号解释成外层 Host 程序的信息。

独立设备 ELF 的 `--verbose` 可能仍只显示元数据。需要完整文件头、段和符号时，可直接读取该设备文件：

```bash
readelf -aW "<device-elf>"
```

### 列出与提取内嵌文件

```bash
msobjdump --list-elf "<input-elf>"
mkdir -p "<artifact-dir>/device-elf"
msobjdump --extract-elf "<input-elf>" --out-dir "<artifact-dir>/device-elf"
find "<artifact-dir>/device-elf" -type f
```

`--out-dir` 需要是已存在的目录；省略时写到当前工作目录。提取后检查实际文件及大小。独立设备 ELF 没有其他内嵌文件时，`nothing to list` 或 `nothing to extra` 是正常提示，直接对该设备 ELF 执行 `--dump-elf` 即可。

一次调用选择 dump、list、extract、sass 中的一种操作，需要多个结果时分别调用。不要把多个主操作拼成一条命令，部分版本会只执行其中一个。

### 反汇编设备指令

安装版帮助包含 `--sass` 时：

```bash
mkdir -p "<artifact-dir>/disassembly"
msobjdump --sass "<input-elf>" > "<artifact-dir>/disassembly/device.txt" 2> "<artifact-dir>/disassembly/stderr.log"
```

`--sass` 输出到标准输出，不能与 `--out-dir`、`--verbose` 或其他主操作组合。SASS 是选项名，实际输出为 Ascend AICore 指令。当前上游支持独立设备 ELF、融合 Host ELF 和普通 `.a` 归档；归档会逐个处理成员，thin archive 不支持。

正常反汇编应包含指令地址、编码、助记符和操作数。只有文件格式标题，或指令全为 `<not available>`，都不能算成功；工具也可能直接报告 `no valid instruction lines`，不转发这类无效输出。此时检查所用 `llvm-objdump` 是否能反汇编该设备架构。多个镜像的输出按 `===== [SASS] 来源 =====` 分段，先看来源再对应 Kernel。

## 元数据怎样解释

按 `.ascend.meta. [索引]: 名称` 分组阅读，保留完整 Kernel 符号；索引相同不代表函数相同。缺少某字段时不要补成默认值。

| 字段 | 可用于确认什么 | 解释边界 |
| --- | --- | --- |
| `.ascend.meta. [索引]: 名称` | 产物中记录的 Kernel 符号，可能经过 C++ 名称修饰。 | 名称存在不代表本轮运行调用过它。 |
| `VERSION` | 元数据格式中的版本值。 | 不是 CANN 安装版本，也不是 msobjdump 软件版本。 |
| `KERNEL_TYPE` | 产物声明的核类型，如 AIC、AIV、MIX_AIC_MAIN、MIX_AIV_MAIN。 | 不能据此推算实际启动核数。 |
| `CROSS_CORE_SYNC` | 硬同步配置，如 USE_SYNC。 | 不能据此证明运行时同步正确。 |
| `MIX_TASK_RATION` | Cube/Vector 任务配比，如 `[1:2]`。 | 不是耗时或利用率比例；保留工具原拼写。 |
| `FUNCTION_ENTRY` | 工具解析出的 TilingKey 整数。 | 不是指令地址，也不证明该 key 本轮被执行。 |
| `BLOCK_NUM` | 当前上游实现打印占位值 `0xFFFFFFFF`；部分安装版不显示此字段。 | 占位值或字段缺失都不能换算成实际执行核数。 |
| `DEBUG`、`RUNTIME_IMPLICIT_INFO` | 调试缓冲区、调试选项及隐式信息标志。 | 有标志不等于本次运行已采集到调试数据。 |

例如混合算子同时出现 `mix_aic`、`mix_aiv` 两个符号，并有 `KERNEL_TYPE: MIX_AIC_MAIN`、`MIX_TASK_RATION: [1:2]`，可据此检查是否生成了预期混合核实现；不能直接把两个符号当作两个独立算子。

## 使用实例：检查融合编译产物

已有融合编译产物时，先列出其内嵌设备文件，再提取和读取元数据。列表或提取目录为空时，先按下文检查融合段，不猜测设备文件名：

```bash
msobjdump --list-elf "<fusion-elf>"
mkdir -p "<artifact-dir>/fusion-device"
msobjdump --extract-elf "<fusion-elf>" --out-dir "<artifact-dir>/fusion-device"
find "<artifact-dir>/fusion-device" -type f
msobjdump --dump-elf "<实际提取出的设备ELF>"
```

当前上游对包含 `.aicore_binary` 的产物会自动提取设备段，文件名通常形如 `<输入文件名>.aicore.o`。以实际列表和生成文件为准；需要完整结构时按上文使用 `readelf -aW`，需要设备指令且工具具备能力时使用 `--sass`。

如果安装版对外层融合 ELF 提示找不到元数据，先检查段信息：

```bash
readelf -SW "<fusion-elf>"
```

确认存在 `.aicore_binary`，但当前工具不能自动处理时，可用配套工具提取后继续解析：

```bash
mkdir -p "<artifact-dir>/fusion-device"
llvm-objcopy -O binary --only-section=.aicore_binary "<fusion-elf>" "<artifact-dir>/fusion-device/device.o"
msobjdump --dump-elf "<artifact-dir>/fusion-device/device.o"
```

没有 `.aicore_binary` 时，不套用这条提取命令；按实际段、符号和当前工具支持的封装类型选择入口。提取成功后可核对 Kernel 名称、核类型与 TilingKey；是否加载该产物、是否选择该分支，仍需本轮运行记录确认。

## 常见问题与结果检查

| 现象 | 处理方法 |
| --- | --- |
| `unrecognized arguments: --sass` | 当前安装未提供反汇编选项；不要换成未声明的 `-sass`。使用支持该选项且与当前 CANN 后端配套的工具版本。 |
| 找不到 Kernel 元数据 | 检查是否给了普通 Host ELF、是否存在设备封装、安装版是否支持该格式；不能直接判定文件损坏或没有设备代码。 |
| 封装层只显示文件信息或 `[ASCEND META]: None` | 用 `--extract-elf` 提取后，对实际设备 ELF 单独执行 `--dump-elf`；该提示不能证明内层没有元数据。 |
| 提取结束但目录为空 | 检查 `nothing to extra`、异常输出和输入类型；不能只凭退出码 0 声称已提取文件。 |
| 输出目录无效或临时目录创建失败 | 先创建输出目录，并检查当前工作目录及输出目录的写权限。 |
| 缺少 `readelf`、`llvm-objcopy` 或 `llvm-objdump` | 在同一 shell 中加载工程采用的 CANN 环境，确认缺少的是哪个依赖；反汇编后端缺失不等于元数据也无法读取。 |
| 路径含空格，命令报参数或文件不存在错误 | 确认 Python 包来自同一套安装后，用 `python3 -m msobjdump` 替换命令前缀，保持参数及路径引号不变，绕过旧 shell 包装脚本的拆词问题。 |
| 有反汇编输出，但同时报错或含未知指令 | 检查完整标准错误和退出状态；部分镜像或指令失败时，已有输出只能算部分结果。 |

保留命令、标准输出、标准错误和实际提取文件。静态元数据和反汇编用于检查编译产物，不能单独证明本轮输入的执行路径、同步正确性或精度根因。

## 官方说明

asc-tools 工具相关详细说明见 [asc-tools 官方仓库](https://gitcode.com/cann/asc-tools)，按 `msobjdump` 查找使用说明与样例。实际操作以当前安装的帮助、格式支持和配套 CANN 工具为准。
