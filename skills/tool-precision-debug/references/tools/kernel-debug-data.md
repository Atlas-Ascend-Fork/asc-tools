# show_kernel_debug_data：解析 Kernel 调测数据

`show_kernel_debug_data` 将 Kernel 内 `AscendC::DumpTensor`、`AscendC::printf` 等接口生成的调测二进制解析为 Tensor 文件、日志和时间戳。

## 使用场景

- 已采集计算前后或搬运前后的 Tensor，需要读取中间值并与局部参考比较。
- 同一处 Tensor 被多个核或多次循环输出，需要按核、标签和记录序号找到对应片段。
- Kernel 的 printf、assert 或时间戳已落盘，需要还原日志，确认执行分支或辅助对齐 Tensor 记录。

输入必须是 Kernel 调测数据；普通输入输出数组即使也以 `.bin` 保存，也不能按这种格式解析。工具只解析已采集的内容，不会自动执行 Kernel、增加观察点或判断算子精度是否达标。

## 采集与解析

使用工程实际采用的 CANN 环境，先运行 `show_kernel_debug_data -h` 确认命令及参数可用。以下示例中的 `<artifact-dir>` 表示本次任务产物目录，执行前替换为实际路径。已有原始调测文件时，直接进入“解析与输出”。

### 配置与采集

Kernel 已包含需要的调试接口调用时，通过工程的 ACL 初始化或框架入口加载以下配置，将调测数据落盘：

```json
{
  "dump": {
    "dump_kernel_data": "tensor,printf",
    "dump_path": "<artifact-dir>/raw"
  }
}
```

`tensor` 导出 DumpTensor 数据，`printf` 导出格式化日志；还可选 `assert`、`timestamp`，或用 `all` 导出全部类型。ACL 直调工程由 `aclInit` 接收配置文件路径；已有框架初始化时使用其配置入口。仅创建 JSON 文件不会启用采集。

需要断言文本时优先选 `all`，并保留程序的运行日志。部分 `ascendc_assert` 消息按 `printf` 类型采集，单选 `assert` 可能漏掉。

采集 `PrintTimeStamp` 时，还需在实际编译 Kernel 的目标上启用 `ASCENDC_TIME_STAMP_ON`。将 `<kernel-target>` 替换为目标名：

```cmake
target_compile_definitions(<kernel-target> PRIVATE ASCENDC_TIME_STAMP_ON)
```

仅将采集类型设为 `timestamp` 或 `all`，不会开启编译时关闭的时间戳打点。

`dump_path` 为必填项，可使用相对路径；核对程序运行目录。路径覆盖优先级为 `ASCEND_DUMP_PATH` > `ASCEND_WORK_PATH` > `dump_path`，找不到文件时先确认实际生效的目录。采集端还会在输出目录下创建 `printf/`、时间戳等子目录；运行带调试调用的 Kernel 后，按实际落盘位置选择本次原始文件。

### 解析与输出

命令接收两个位置参数：

```text
show_kernel_debug_data <bin_file_path> [output_path]
```

| 参数 | 含义 |
| --- | --- |
| `bin_file_path` | 必填。一个原始调测文件，或包含原始调测文件的目录；目录模式递归收集 `.bin`。 |
| `output_path` | 可选。解析结果目录，不存在时自动创建；省略时使用当前工作目录。建议显式指定。 |

解析单个文件：

```bash
show_kernel_debug_data "<raw-bin-file>" "<artifact-dir>/parsed-one"
```

解析同一次 Kernel 调用的调测目录：

```bash
show_kernel_debug_data "<artifact-dir>/raw/<本次调测目录>" "<artifact-dir>/parsed"
```

目录模式只传入本次调用的数据，避免混入其他运行或 Kernel 的文件；相同核、标签和记录序号可能生成同名输出。解析目录放在原始输入目录之外，防止下次递归解析时把已提取的 Tensor `.bin` 再当作原始调测文件。

## 读取解析结果

工具在输出目录下创建 `PARSER_<时间戳>/`，并打印实际位置。常见产物如下；只有输入包含相应记录时才会生成对应文件。

| 产物 | 怎样使用 |
| --- | --- |
| `parser.log` | 查看解析日志；结合终端标准输出和错误输出检查异常。 |
| `dump_data/<核标识>/` 下的 Tensor `.txt` | 快速观察数值和形态；有 shape 信息时按 shape 排列，没有时为顺序数值。 |
| 同名 Tensor `.bin` | 提取出的 Tensor 原始数据字节。按实际 dtype 读取，用于数值或逐位比较；它不再是带调测记录头的输入文件。 |
| `dump_data/index_dtype.json` | 查看 `desc` 标签对应的 dtype；它不提供完整的逻辑 shape、layout 或全局坐标映射。 |
| `time_stamp_core_<核标识>.csv` | 查看采集到的时间戳记录。 |
| 终端中的 `block.<核标识> begin/end` 段 | 查看对应记录的 printf、assert 等文本。 |

具体目录层级和文件前缀随调测格式而异，定位时关注以下字段：

- **核标识**：用于关联原始文件、解析目录与日志，不保证等同于 `AscendC::GetBlockIdx()`。文件名中的 `aic`、`aiv` 也不宜单独作为核运行类型的依据。需要对应逻辑 block 时，在同次采集的 printf 中输出 `GetBlockIdx()`，再与解析日志配对。
- **index**：对应 `DumpTensor` 的第二个参数 `desc`，不是 Tensor 元素下标。不同 Tensor 使用不同标签，避免记录混在一起。
- **loop**：同一核、同一标签在所解析记录中的出现序号，从 0 开始。条件打印、重复标签或多次 Kernel 调用都会影响对应关系，不能直接当作源码循环变量。

比较时保留 `.bin` 并确认 dtype、实际采集元素数和布局；`.txt` 用于阅读，不用于恢复原始位模式。全局坐标需要结合 Kernel 的核分片、循环偏移及 DumpTensor 的起始偏移计算。

## 使用实例：读取 Add 的输入与输出片段

下面以官方 Add 样例的调测数据说明读取方法。样例将 `xLocal[64]`、`yLocal[64]`、`zLocal[64]` 起始的各 16 个 float32 元素分别标记为 `desc=0`、`1`、`2`。已有该样例一次运行的原始调测文件时：

```bash
show_kernel_debug_data "<本次样例调测目录>" "<artifact-dir>/add-parsed"
```

先找到各核标签 2 的第一条记录：

```bash
find "<artifact-dir>/add-parsed" -type f -name '*_index_2_loop_0.bin'
```

选择其中一份，在同目录下按共同前缀和相同 `loop` 找到输入与输出。下面的 `<共同前缀>` 取自实际文件，不固定核标识或 `aic`、`aiv` 前缀：

```text
<共同前缀>_index_0_loop_0.bin  # x 的片段
<共同前缀>_index_1_loop_0.bin  # y 的片段
<共同前缀>_index_2_loop_0.bin  # z 的片段
```

先读同名 `.txt` 查看值，再按 float32 读取三份 `.bin` 做局部比较。每份应有 16 个元素，即 64 字节；比较同一核、同一组记录中 `z[i]` 与 `x[i] + y[i]`，按项目判据判断差异。这里的元素 `i=0` 对应当前 tile 的偏移 64，不能直接对照完整输入数组的第 0 项。其他循环组也按相同的核、标签和记录顺序配对。

这组数据只能检查已采集的 16 个元素；样例程序的精度验收与解析命令是否成功需分别判断。

## 判断解析是否成功

同时检查命令退出状态、异常输出和实际产物，再核对目标核、标签、记录数及每条 Tensor 的数据量。只采集 printf 时没有 Tensor 文件是正常情况；需要 Tensor 却只有日志时，检查采集类型和实际执行的调试调用。

| 现象 | 处理方法 |
| --- | --- |
| 找不到输入或目录中没有 `.bin` | 核对配置是否加载、路径覆盖和本次运行的实际落盘目录。 |
| `unknown block magic`、`file too small` 或 TLV 长度异常 | 确认输入为完整的原始调测文件，未混入普通数组或已解析产物；再核对采集端与解析器是否配套。保留原文件，不靠改扩展名或裁剪文件头修复。 |
| 文本出现占位 `-`，或日志提示元素不足/多余 | 检查采集元素数与记录的 shape 是否匹配；不要把占位当作零。shape 小于实际数据量时，文本可能只显示一部分，二进制仍保留实际采集的字节。 |
| 报输出路径含非 ASCII 字符 | 改用 ASCII 字符组成的输出路径后重试。 |
| 路径含空格时被拆成多个参数 | 在能导入配套工具包的 Python 环境中，改用 `python3 -m show_kernel_debug_data "<raw-bin-file>" "<artifact-dir>/parsed-one"`，绕过 shell 包装脚本的参数转发。 |
| 已生成部分结果，但命令报错或缺少预期记录 | 按异常定位对应输入文件；目录解析可能在中途失败，已有文件不能证明整个目录已解析完整。 |

## 官方说明

asc-tools 工具相关详细说明见 [asc-tools 官方仓库](https://gitcode.com/cann/asc-tools)，按 `show_kernel_debug_data` 查找使用说明与样例。安装版本与当前文档不一致时，以配套工具帮助和对应版本说明为准。
