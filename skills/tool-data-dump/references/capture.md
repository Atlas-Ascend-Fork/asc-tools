# Runtime Dump 配置与工程接入

本页对应主流程阶段 2A/2B，说明配置选择、工程接入时序及采集结果的核验方法。根据阶段 1 的运行摘要和本轮采集目标，准备配置并执行工程，保留配置文件、运行命令、日志及原生 CSV 或 raw，同时记录数据所属的请求、模型和调用。需要分析时，CSV 进入阶段 3，raw 进入阶段 4。

采集准备与初始化参数优先查阅 [Runtime 官方 Raw 配置文档](https://raw.gitcode.com/cann/runtime/raw/master/docs/zh/api_ref/02_initialization_and_deinitialization.md)中的“模型Dump配置、单算子Dump配置”。该文档覆盖 `aclInit`、`dump_data`、`dump_list` 与模型/单算子配置示例，可通过普通 HTTP 获取 Markdown。

完整字段集中在[acl.json 参数参考](acl-json.md)，包含 stats、自选统计项、范围和黑名单，可按采集目标查阅。Raw 基础示例未列出全部字段，不能据其缺项就认定本机不支持；实际参数与产品限制以执行端配套 CANN 资料为准。

## 确认工程接入点

1. 在 Host 代码中确认首次 `aclInit(configPath)`、配置路径的传入方式、模型加载时机，以及执行和 stream 同步的位置。工程已有 `acl.json` 配置入口时，直接复用，不修改数值 Kernel。
2. 按实际执行方式分流：通过模型加载/执行接口运行计算图时走 2A；ACLNN 等单算子 API、单算子模型执行及 `<<<>>>` 直调走 2B。单算子模型也可能是 `.om`，不能凭扩展名分类。模型用 `model_name`、必要时用 `layer` 选层；单算子开启 `dump_op_switch`。直调还需按下节补齐 Host 显式张量 Dump。
3. 重复调用在不改变复现条件的前提下，优先在新进程只重跑目标请求。依赖预热、前序调用或累积状态时保留这些步骤，记录目标调用身份。模型/API 使用显式窗口时，确认它在目标内部模型首次加载前生效，已缓存的旧模型未必受中途配置影响。
4. 已有 `.om` 但找不到融合后层名时，优先查阅工程已有的模型信息。必要时按[ATC 离线模型转 JSON](https://raw.gitcode.com/cann/ge/raw/master/docs/zh/user_guides/atc_tools/basic_feature/basic_features.md)及当前工具帮助从**已有 `.om`** 导出，例如 `atc --mode=1 --om=<已有模型.om> --json=<模型信息.json>`，核对模型顶层名称、节点和 NetOutput 连接。此操作用于映射 Dump 张量，不制作 golden，也不重新转换模型。

**确认结果**：记录目标模型/API/kernel 调用、请求、已知层名、配置入口和采集方向。直调场景应根据接口与张量描述判断采集条件。缺少可用接口或可靠描述时，记录具体缺项；不能仅依据 `acl.json` 判断采集能力，也不能一概排除直调场景。

## kernel 直调的 Host 接入

[aclDumpOpTensors 官方说明](https://raw.gitcode.com/cann/opbase/raw/master/docs/zh/api/nnopbase/aclnn/aclDumpOpTensors.md)给出了 `<<<>>>` 调用后的 Host Dump 示例。`aclInit` 负责启用和配置 Dump；`aclDumpOpTensors` 负责提交具体张量的描述及采集任务。它可以与 `aclInit` 配置配合使用，不属于另一套 Dump 初始化流程。

检查执行端头文件 `aclnn/acl_meta.h`、配套库接口及工程链接设置，确保头文件与库来自同一套 CANN，并以当前安装确认接口可用性。

准备独立目录，以本页单算子配置 C/D 设置 `dump_op_switch=on`、`dump_mode` 和 `dump_data`，传入首次有效的 `aclInit`。官方示例有时省略开关，这不代表可以省略启用步骤；返回 0 但没有目标文件仍是未采到。stats 支持情况应通过本机生成的 CSV 核验；tensor 可用不代表所有版本、设备或图捕获场景均支持 stats。

从工程已有元数据创建 `aclTensor`，准确填写逻辑 shape、实际 dtype、stride、viewOffset、format、storage shape 和设备地址。`tensors` 数组先放输入，再放输出，两个计数与数组一致。下列是调用时序示意，张量、名称、launch 参数和接口错误处理须使用工程实际实现：

```cpp
// 此前已通过 aclInit(configPath) 启用 Dump，并准备好张量与 stream。
// check 表示工程已有的错误处理；inputTensors/outputTensors 由真实元数据创建。
check(aclDumpOpTensors(opType, inputRecordName, inputTensors,
                      inputCount, 0, stream));
// 此处仅以第二项为 0 的调用示意；实际保留原工程的全部启动配置和实参。
kernel<<<blockDim, 0, stream>>>(/* 原工程实参 */);
check(aclDumpOpTensors(opType, outputRecordName, outputTensors,
                      0, outputCount, stream));
check(aclrtSynchronizeStream(stream));
// 同步完成后按原工程生命周期销毁描述对象、释放设备内存。
```

只采输出时可省略输入 Dump 调用；同时采两侧用 `dump_mode=all`。`opType` 对应实际类型，记录名称关联真实请求、调用及方向；保存这些名称和 raw 的对应关系，不将自定义标签作为模型层名。Dump 与目标 kernel 使用同一 stream；若数据由其他 stream 产生，先沿用工程已有依赖保证就绪。描述和设备内存至少保持到相关任务完成。

确认所有输入在计算后仍保持不变时，也可按官方示例在 launch 后一次传入输入和输出。**原地计算、输入输出别名或缓冲复用时，必须在覆盖之前采输入，之后采输出**；事后构造一个标为 input 的描述不会恢复旧数据。这里只增加 Host 的输入输出采集，不改变计算 Kernel，也不插入内部变量 DumpTensor。

先用小范围验证接口返回、本轮 raw/CSV、输入输出索引、shape/dtype 和真实数据，再复用普通的 stats 筛查、`msaccucmp convert/file_compare` 流程。直调缺少图中间节点映射时，按 Host 调用和张量配对，不为使用整网 compare 虚构模型结构。

## `acl.json` 的字段怎样组合

配置时分别确定四个维度：**对象**（模型/单算子和层）、**方向**（输入/输出）、**内容**（统计/张量）及**层级**（op/kernel）。各字段按其定义使用，避免混淆作用范围。

| 采集目标 | 选项及下一步 |
| --- | --- |
| 模型全量数据 | `dump_list:[{}]`；数据量大时先确认是否可按模型、节点或范围缩小 |
| 指定模型或离散节点 | 设置真实 `model_name`，按需增加 `layer`；文件加载与内存加载的名称来源见[参数参考](acl-json.md#model_name) |
| 模型的一段或多段节点 | 配置 `opname_range`，使用非空模型名和转换后的非 Data 端点，且 `dump_level=op` |
| 排除某类型或某名称的输入输出 | 分别使用 `optype_blacklist` 或 `opname_blacklist`；仅模型、op 级生效，`pos` 指定具体索引 |
| 单算子模型/API 调用 | `dump_op_switch=on`、`dump_list:[{}]`，按工程入口限定目标请求；范围和黑名单不能当作单算子调用次数过滤器 |
| 核对具体误差坐标 | 采集 `tensor`；确认输入输出边界时用 `dump_mode=all`，随后解析并比对 |
| 从大量可比观察点中筛选候选 | 先采集 `stats`；支持时用 `dump_stats` 自选指标，再对候选和必要输入补采 tensor |

| 字段 | 使用场景 | 注意事项与核验方法 |
| --- | --- | --- |
| `dump_list`、`model_name`、`layer` | 模型场景列出实际模型；已知候选节点或全量文件过多时加 `layer`。不写 `layer` 表示扩大到该模型可采的层 | 文件加载通常填 `.om` 文件名去扩展名，或 ATC JSON 顶层 `name`；内存加载按 JSON 顶层 `name` 核对。融合、拆分后节点名可能变化；`layer` 不能指定“同名 API 的第 3 次调用” |
| `dump_op_switch` | 模型 Dump 用 `off`，受支持的单算子调用用 `on` | 单算子开关不限定具体调用；存在多次调用时，通过单请求重跑或采集窗口限定目标 |
| `dump_path` | 两种场景都填；运行前创建目录并核对写权限 | 相对路径以被测进程工作目录为基准。stats、tensor 及不同请求使用新目录，避免把上次文件当本次证据 |
| `dump_mode` | `output` 用于低成本初筛；确认首个异常边界时用 `all` 采全部可见输入输出；只追输入来源可用 `input` | `all` 的输入可能在执行前、输出在执行后分别落盘，文件数不等于调用数 |
| `dump_data` | `stats` 输出摘要 CSV，适合大范围筛查；`tensor` 输出原始张量，适合逐元素确认 | stats CSV 不包含完整张量；切换采集内容时，保持输入一致并使用新目录 |
| `dump_level` | 当前配置说明的默认值为 `all`；目标在算子层可见时选 `op`，需要实现层输入输出时再选 `kernel`/`all`，执行端版本有差异时按配套说明 | stats 与 tensor 补采保持待比对层级一致；`kernel` 级别不采集任意局部变量；调整层级后须核对目标文件是否完整 |
| `dump_stats` | 仅 `dump_data=stats` 且本机版本、产品支持自选项时指定，例如 Max、Min 或 L2norm | 不支持时省略该字段，查看实际 CSV；不能将缺少 L2norm 写成零，也不能据此断言整个 stats 模式不可用 |
| `dump_step` | 按版本用于训练迭代筛选 | 推理场景无需配置；它不是业务请求号，也不是重复单算子调用的序号 |
| `opname_range` | 模型、op 级采连续闭区间，可配置多段；`model_name`、`begin`、`end` 必须非空 | 端点是 ATC 转换后的非 Data 算子；它控制采集范围，不是后续比对工具的范围选项 |
| `optype_blacklist`、`opname_blacklist` | 模型、op 级按转换后的类型或名称排除指定输入输出；两者可配合使用，各最多 100 项 | `pos` 使用 `inputn`/`outputn`；空 name 或 pos 项不生效。确认未排除本次需要核验的输入 |

`dump_scene` 的异常算子/Watcher、`dump_debug` 溢出、`dump_kernel_data` Kernel 调测信息用于不同问题，且部分模式不能与常规模型/单算子 Dump 并开。本 Skill 的主线是正常执行时的输入输出与 golden 比对；遇到这些配置先辨明诊断目标，不将产生的不同格式文件交给本流程误解析。

## 四份可改写的配置

**A. 采集指定模型的多层输出统计。** 适用于中间 golden 覆盖较广、全量 tensor 采集成本较高的模型。`实际模型名` 必须来自本次加载的模型，不能使用自定义的业务别名。

```json
{
  "dump": {
    "dump_path": "debug/run-a/stats",
    "dump_list": [{"model_name": "实际模型名"}],
    "dump_op_switch": "off",
    "dump_mode": "output",
    "dump_level": "op",
    "dump_data": "stats"
  }
}
```

本例假设执行端支持 `dump_level=op` 且 golden 对应算子级输出；先核对真实 CSV 中有目标点。若目标只在其他层级可见，就按本机支持改用 `all`/`kernel`，不能仅为减少文件数量而遗漏目标数据。如果全层 CSV 仍过多，使用 `layer` 选择离散节点，或按[范围配置](acl-json.md#opname_range)用 `opname_range` 缩小采集范围。

**B. 模型目标层的输入输出 tensor。** 目标范围已明确且采集成本可接受时可直接使用，也可在 A 的统计筛查后用于补采。填入真实目标层名；要核验输入是否正常，`dump_mode` 需要覆盖输入。下例沿用 A 的 `op` 层级，实际按待比对观察点调整。

```json
{
  "dump": {
    "dump_path": "debug/run-a/tensor",
    "dump_list": [{"model_name": "实际模型名", "layer": ["实际候选层名"]}],
    "dump_op_switch": "off",
    "dump_mode": "all",
    "dump_level": "op",
    "dump_data": "tensor"
  }
}
```

**C. 采集已确定单算子调用的 tensor，用于核对差异坐标。** 默认采用 `aclInit` 配置说明中的 `dump_list:[{}]`；另一些官方示例使用 `[]`，遇到已有工程采用该写法时，按执行端配套说明和真实采集核对，不单凭空列表判定配置无效。单算子文件通常不经过模型层目录，目标请求在保持复现条件的前提下，优先通过新进程单请求运行来限定。

```json
{
  "dump": {
    "dump_path": "debug/request-a/tensor",
    "dump_list": [{}],
    "dump_op_switch": "on",
    "dump_mode": "all",
    "dump_data": "tensor"
  }
}
```

**D. 多次单算子调用有对应 golden 时，先采集统计进行筛查。** 与 C 的区别只在采集内容和方向。仍应限定请求，或为每次调用保留 Host 调用记录；需要核验候选张量的差异坐标时，按 C 补采 tensor。目标明确且数据量较小时，直接使用 C。

```json
{
  "dump": {
    "dump_path": "debug/request-a/stats",
    "dump_list": [{}],
    "dump_op_switch": "on",
    "dump_mode": "output",
    "dump_data": "stats"
  }
}
```

这些目录均只是相对路径示例。将 JSON 保存到工程允许的位置，提前创建 `dump_path`，再通过工程**实际**参数将配置传入运行进程；不能假定所有工程都提供 `--acl-config` 选项。一次采集只运行复现所需请求，输入和模型版本与 golden 保持一致；若异常依赖前序调用或累积状态，保留这些步骤并记录调用标识，不能为隔离目标而改变复现条件。

## Dump 启用与关闭时序

| 工程入口 | 正确时序 | 易错点 |
| --- | --- | --- |
| 首次 `aclInit` 接收配置路径 | 准备目录与 JSON → 首次 `aclInit(configPath)` → 加载模型/准备单算子 → 执行并同步 → 按工程原流程释放资源 | 连续多次 `aclInit` 时只有首次配置生效；后续调用不能用于切换 stats/tensor |
| 已初始化工程需限定窗口 | `aclInit` 未启用 Dump → `aclmdlInitDump()` → `aclmdlSetDump(configPath)` → 加载目标模型/准备单算子 → 执行并同步 → `aclmdlFinalizeDump()` → 按原流程卸载和释放 | 不与 `aclInit` Dump 叠加；配置只影响设置之后加载的模型，旧模型句柄要卸载并重载；异步任务完成后再结束采集窗口 |

逐项检查 `aclInit`、Dump 初始化、设置与结束接口的返回值；失败时记录实际失败步骤。当前 Runtime 的显式 Dump 接口依赖 `acl/acl_dump.h` 与 `libascend_dump.so`，初始化和 stream 等 Runtime 接口依赖 `acl/acl_rt.h` 与 `libacl_rt.so`；已有工程可能使用兼容入口，应沿用同一套 CANN 的依赖。`aclDumpOpTensors` 属于另一个接口组，按前面的 Host 接入核对 `aclnn/acl_meta.h` 和配套库，不能将 Runtime Dump 库当作所有接口的依赖。版本或符号不符时查[aclmdlSetDump 官方说明](https://raw.gitcode.com/cann/runtime/raw/master/docs/zh/api_ref/18_dump_configuration.md)及[官方依赖说明](https://raw.gitcode.com/cann/runtime/raw/master/docs/zh/api_ref/01_overview.md)，不修改计算 Kernel。

模型多次推理且允许串行复现时，在异步推理后同步，便于确认请求与文件的对应。必须保留并发或前序状态才能复现时，沿用原依赖并记录调用身份。重复单算子调用如已触发内部模型缓存，中途启用采集窗口未必对已加载模型生效；可从新进程开始启用 Dump，同时保留所需的前序调用。补采需保持目标输入和计算参数，并核对异常是否仍复现，跨轮行为不同就分别记录。

## 采集结果核验

模型推理通常按 `{dump_path}/{时间}/{device_id}/{model_name}/{model_id}/{data_index}/` 组织；单算子调用通常按 `{dump_path}/{时间}/{device_id}/` 组织，实际路径以当前版本和真实文件为准。文件名常包含算子类型、名称、task、stream、时间戳。`data_index` 是每个 Task 的 Dump 次数序号，**不是**业务 request ID。融合或特殊字符可能改变文件名；不按目录排序推断依赖关系。

stats 是 CSV；逐行看 `Input/Output`、`Index`、`Shape`、`Data Type`、`Format`、`Max Value`、`Min Value`、`Avg Value`、`Nan Count` 和正负 Inf 计数，实际还可能有 `l2norm`。tensor 是带元数据的原始 Dump，使用官方 `msaccucmp convert` 转换为 NPY 后读取内容。同一 raw 可含多个 `input.0`、`input.1` 或 output；单算子输入与输出也可能分别落在文件名不同、task 编号不同的 raw 中。先根据工程调用记录、方向和索引建立对应关系，再进行数值比对。

阶段 2 核对进程最终状态、目标目录新文件和日志中的请求/调用归属。CSV 在阶段 3 核对方向与索引；raw 在阶段 4 解析后确认具体张量，尚未解析的文件保留待确认状态。**目标张量采集成功需要三项同时成立**：进程本轮真实完成、目录有本轮文件、文件内容能对应到目标请求/层/方向/索引。仅有目录、配置解析成功或进程返回 0，均不足以确认采集成功。

## 采不到预期文件时

| 现象 | 先查什么 | 可采取的最小动作 |
| --- | --- | --- |
| 目录空或文件在别处 | 首次 `aclInit` 是否拿到配置、相对路径按哪个工作目录解释、环境变量是否改变落盘目录、路径权限 | 固定一个新目录和一次请求，查看接口返回与运行日志，确认新文件时间和路径 |
| 模型有输出却没有指定层 | `model_name/layer` 是否匹配实际加载模型、融合后名称、目标节点是否真正执行 | 从现有 `.om` 的模型信息核对节点；缩小到有执行任务的邻接层重采 |
| 单算子没有文件 | 当前调用方式与产品是否支持、`dump_op_switch`、`dump_list` 的版本写法、Dump 生命周期是否覆盖目标 | ACLNN 等模型/API 路线优先新进程、内部模型首次加载前启用；直调另查 Host 是否实际调用 aclDumpOpTensors、张量描述及 stream 是否正确；接口返回 0 不保证已启用采集 |
| 有 CSV 但缺某统计列 | 当前设备及版本对 `dump_stats` 的支持、CSV 表头及数据类型 | 省略不支持的自选项或记录缺口；补采 tensor，不将缺列记零 |
| 文件很多却找不到某次请求 | 多请求共享目录、异步执行、同名 API 重复、`data_index` 与请求号混淆 | 单请求单目录重采，保存 Host 请求与实际文件的对应关系 |
| 换成 tensor 后内容没变或仍是旧文件 | 新旧目录是否隔离、模型是否在新配置后重载、输入是否仍相同 | 新目录重跑，核对运行最终状态和模型加载顺序，不覆盖旧证据 |

原生 stats 不可用时，记录失败步骤和已完成的排查，然后按需继续 tensor 路线。由 tensor 计算的统计应单独标注，不能计作 Runtime 原生 CSV 采集结果。
