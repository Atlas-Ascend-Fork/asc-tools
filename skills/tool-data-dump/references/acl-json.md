# acl.json 模型与单算子 Dump 参数

本页汇总常规输入输出 Dump 的配置字段、默认值及适用限制。Runtime 基础接口与模型/单算子 JSON 示例见[官方配置文档](https://raw.gitcode.com/cann/runtime/raw/master/docs/zh/api_ref/02_initialization_and_deinitialization.md)；该基础文档未列出全部可选字段。执行时核对配套 CANN 版本、NPU 产品和工程入口，主线资料中的选项不代表本机均支持。

以下字段放在根对象的 `dump` 对象中；`model_name`、`layer`、两种 blacklist 和 `opname_range` 放在 `dump_list` 的模型条目中。配置选择与加载方式见[配置与采集](capture.md#四份可改写的配置)。示例中的模型名、算子名和目录均需按工程填写。

## dump_list

必选，待 Dump 数据的整网模型列表。

- 模型推理需要采全部模型的全部算子时，配置 `"dump_list": [{}]`。
- 采多个模型或指定算子时，结合 `model_name` 和 `layer` 配置。
- 单算子模型执行、单算子 API 执行时，建议配置 `"dump_list": [{}]`，并启用 `dump_op_switch`。

## model_name

模型名称，各模型条目的 `model_name` 值须唯一。

- 文件加载：使用模型文件名，不带后缀；也可使用 ATC 模型 JSON 最外层的 `name` 字段。
- 内存加载：使用 ATC 模型 JSON 最外层的 `name` 字段。
- `layer` 和 blacklist 可在不设置 `model_name` 时作用于所有模型；`opname_range` 必须设置非空的 `model_name`。

## layer

指定需要 Dump 的算子名，使用字符串数组，每个元素是一个算子名，以英文逗号分隔。支持 ATC 转换后的算子名，也支持转换前的原始算子名。

- 设置 `model_name` 时，只采该模型下的对应算子；不设置时，采所有模型下的对应算子。
- 采模型全部算子时，省略 `layer`。
- 指定算子的输入涉及 Data 算子时，会同时 Dump Data 算子信息。需要采 Data 节点数据时，要一并填写其后继节点。
- IO 性能较低或数据量较大时，优先指定目标算子，防止全量 Dump 导致超时。
- 该字段按算子名称筛选，不能指定重复 API 的某一次调用；重复调用须结合工程请求与调用记录区分。

## optype_blacklist

按算子类型排除指定输入或输出，控制采集量。仅模型 Dump 且 `dump_level=op` 时生效，可与 `opname_blacklist` 配合使用。

- `name`：ATC 转换后的算子类型，空值时该过滤项不生效。
- `pos`：需排除的位置数组，格式只能是 `inputn` 或 `outputn`，`n` 为输入输出索引；空值时该过滤项不生效。
- 最多 100 个过滤项。
- 有 `model_name` 时仅作用于该模型，无 `model_name` 时作用于全部模型。

## opname_blacklist

按算子名称排除指定输入或输出。仅模型 Dump 且 `dump_level=op` 时生效，可与 `optype_blacklist` 配合使用。

- `name`：ATC 转换后的算子名，空值时该过滤项不生效。
- `pos`：需排除的位置数组，格式只能是 `inputn` 或 `outputn`；空值时该过滤项不生效。
- 最多 100 个过滤项。
- 有 `model_name` 时仅作用于该模型，无 `model_name` 时作用于全部模型。

下面同时演示两种黑名单：排除 `Conv2D` 类型的 input0/input1，以及 `relu1` 节点的 output0。示例名称用于说明配置格式，使用时应根据模型信息替换，并保留本次比对所需的输入输出。

```json
{
  "dump": {
    "dump_list": [{
      "model_name": "ResNet-50",
      "optype_blacklist": [{"name": "Conv2D", "pos": ["input0", "input1"]}],
      "opname_blacklist": [{"name": "relu1", "pos": ["output0"]}]
    }],
    "dump_path": "debug/filtered/tensor",
    "dump_mode": "all",
    "dump_level": "op",
    "dump_data": "tensor"
  }
}
```

## opname_range

按算子名称指定采集范围，采集从 `begin` 到 `end` 的闭区间。仅模型 Dump 且 `dump_level=op` 时生效。

- `model_name` 必须非空。
- `begin` 和 `end` 使用 ATC 转换后的算子名称，均不得为空，且只能指定非 Data 算子。
- 区间内算子的输入涉及 Data 算子时，会同时 Dump Data 算子信息。
- 可配置多个区间。该字段定义模型的数据采集范围，不适用于筛选单算子 API 的某一次调用，也不等同于 msaccucmp 的比对范围参数。

```json
{
  "dump": {
    "dump_list": [{
      "model_name": "ResNet-50",
      "opname_range": [
        {"begin": "conv1", "end": "relu1"},
        {"begin": "conv2", "end": "pool1"}
      ]
    }],
    "dump_path": "debug/model-range/tensor",
    "dump_mode": "output",
    "dump_level": "op",
    "dump_data": "tensor"
  }
}
```

## dump_path

必选，运行环境的 Dump 存储目录。运行前创建目录，确保实际运行用户有读写权限。

- 绝对路径以 `/` 开头。
- 相对路径直接以目录名开始，例如 `output`，以运行进程的工作目录为基准。
- 每轮采集使用独立目录，stats 与补采的 tensor 分别保存，便于确认数据来源。

## dump_mode

采集方向：`input` 为输入，`output` 为输出，`all` 为输入和输出；默认 `output`。

部分算子会修改输入，例如集合通信算子。使用 `all` 时，输入可在算子执行前采集，输出在执行后采集，因而分别落盘、产生多个文件。解析后根据内容确认方向，不能将文件数量当成调用数量。

## dump_level

采集层级：`op` 为算子级，`kernel` 为 kernel 级，`all` 为两种级别；默认 `all`。

默认配置文件较多，可能出现 `aclnn` 开头的文件。目标在算子级可见、需要降低 IO 或内存开销时，可选 `op`；同时核对实际产物，确认目标数据仍在采集范围内。算子是逻辑运算，kernel 是设备上的具体实现；kernel 级输入输出仍不等于 Kernel 内部变量 Dump。

## dump_op_switch

单算子调用场景的采集开关，包括单算子模型执行和单算子 API 执行。`on` 为开启，`off` 为关闭；默认 `off`。

按执行接口判断模型或单算子路线，不能仅根据 `.om` 后缀判断。kernel 直调还需按[Host 接入](capture.md#kernel-直调的-host-接入)提交具体张量，不能仅靠 JSON 假定已采到。

## dump_step

选择训练迭代，推理场景无需配置。不配置时默认所有迭代都采集。

- 多个迭代用 `|` 分隔，范围用 `-`，例如 `"dump_step": "0|3-5|10"`。
- 训练时若同时通过 `GEInitialize` 的 `ge.exec.dumpStep` 配置迭代范围，以最后配置的值为准。
- 迭代号不是业务请求号，也不是重复单算子调用的序号。

## dump_data

采集内容：`tensor` 为算子数据，`stats` 为算子统计数据；默认 `tensor`。

- `tensor` 保存原始张量 Dump，用于解析后与 golden 逐元素比较。
- `stats` 保存 CSV，包含算子名、输入输出元数据和统计摘要。数据量大且有可对应参考时，可以先筛查统计，再补采候选张量。

## dump_stats

仅 `dump_data=stats` 时使用，选择采集哪些统计项。参数说明中列出的支持产品为 **Atlas A2 训练系列产品 / Atlas A2 推理系列产品**；执行前仍须核对配套版本和设备资料。不支持自选统计项，不等于不支持 stats 模式。

未设置时，默认采集 Max、Min、Avg、Nan、Negative Inf、Positive Inf；L2norm 不在默认列表中。

| 取值 | 含义 |
| --- | --- |
| `Max` | 最大值 |
| `Min` | 最小值 |
| `Avg` | 平均值 |
| `Nan` | NaN 统计，仅针对 half、bfloat、float 浮点类型 |
| `Negative Inf` | 负无穷统计，仅针对上述浮点类型 |
| `Positive Inf` | 正无穷统计，仅针对上述浮点类型 |
| `L2norm` | L2 范数 |

执行端支持自选统计项时，可参考以下配置。JSON 取值须使用规定的拼写；CSV 列名及字段可用性以实际产物为准。

```json
{
  "dump": {
    "dump_list": [{}],
    "dump_path": "debug/model/stats",
    "dump_mode": "output",
    "dump_data": "stats",
    "dump_stats": ["Max", "Min", "Avg", "Nan", "Negative Inf", "Positive Inf", "L2norm"]
  }
}
```

未采集或不支持的统计项应标记为不可用，不能按零处理。CSV 的用途和补采路线见[统计筛查](compare.md#统计筛查怎样读)。
