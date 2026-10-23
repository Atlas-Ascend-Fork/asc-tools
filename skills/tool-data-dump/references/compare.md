# Dump 数据解析与精度比对

本页说明 Dump 数据的解析、比对和结果解读方法。首先区分 CSV 与原始 tensor Dump，确认其对应请求和张量；有 golden 时，再比较差异并形成可复核的数据层结论。**Runtime 原生 stats 是一份张量的摘要，msaccucmp 指标是两份张量的比较结果**；工程 PASS/FAIL 还取决于已有的完整判据。

| 已有数据 | 处理方式 | 交付内容 |
| --- | --- | --- |
| 原生 stats CSV | 直接读统计列；有对应 golden 时计算同名统计并比较，选出需要补采的点 | 统计差异、候选和具体补采范围；没有异常坐标，也不能单凭统计一致判通过 |
| 原始 tensor Dump | `convert` 转 NPY，确认方向、索引、shape、类型表示和布局；需要比对时再配对 golden | 可读取的实际数组、来源关系和转换缺项；比对所需的可信配对 |
| 已转换并可信配对的 NPY | `file_compare` 获取官方指标，再按工程判据补充逐元素差异 | 超差数量及比例、误差、坐标、双方值和完整掩码 |
| 只有实际数据，没有对应 golden | 记录真实统计和数据现象，交付采集结果并列出缺少的参考 | 说明采集范围及实际数值表现；精度与首异常边界保持未判定 |

目标明确时，可按“采集目标 tensor → 使用官方工具转换为 NPY → 配对 golden → 官方比对与逐元素检查 → 输出报告”的顺序处理。范围较大且需要筛选候选时，先执行“采集 stats → 确认候选 → 定向补采 tensor”。仅采集的任务无需准备或生成参考数据。

| 主流程阶段 | 本页入口 | 输入与输出 |
| --- | --- | --- |
| 3. 统计筛查 | [统计筛查怎样读](#统计筛查怎样读) | 原生 CSV → 统计现象；有 golden 时补充差异和候选，不用 `convert` 解析 CSV |
| 4. 解析与配对 | [官方工具怎样选](#官方工具怎样选)、[对应关系](#先建立可复核的对应关系) | raw + 张量元数据 → NPY 及来源关系；需要比对时再确认 golden 配对 |
| 5. 精度比对 | [官方工具怎样选](#官方工具怎样选)、[辅助脚本](#辅助脚本的输入输出) | 可信配对 + 判据 → 官方指标、逐元素明细与完整掩码 |
| 6. 报告 | [报告骨架](#报告骨架) | 已完成的比较与缺口 → 有明确覆盖范围的数据层报告 |

本页提供常规命令和辅助脚本的输入输出；外链用于核对版本支持及高级功能。`<cann-root>` 是执行端实际使用的 CANN 安装目录；尖括号都是待替换说明，不是可直接执行的路径。

## 先建立可复核的对应关系

对每个待比较的观察点，记录以下信息：本次请求/模型/调用、输入或输出、索引、实际 Dump 文件、转换后的 NPY、golden 的文件与 key、shape、原始 dtype、布局及有效元素范围。配对要求是同一复现请求、相同原始输入下语义对应的张量；中间调用的实际输入是否偏离 golden 是待验证的结果，不能将“中间值已相等”当作配对前提。若模型发生融合，结合已有 ATC 模型 JSON、NetOutput 连接及输出索引确认对应；无法唯一确定就标为未匹配，不得根据文件顺序、时间戳或数值相似度推测对应关系。

同一个原始 Dump 文件可能转换出多个 `input.0`、`input.1` 和 `output.0`；一个单算子调用的输入、输出也可能在不同 raw 中。文件数不等于张量数或调用数。BF16 等原始 dtype 在官方转换后可能以另一 dtype 的 NPY 表示，报告保留原始类型及转换说明；不能通过任意 reshape、裁剪、广播或降精度消除差异。shape 相同但布局或输入顺序不同也不能直接比较。非连续张量还要根据 Host 记录核对逻辑 shape、stride 和 viewOffset；现有辅助脚本不重建复杂 view，无法证明转换后 NPY 与 golden 表示相同逻辑元素时标为未匹配。可选输出传 `nullptr` 时，先核对调用实参和实际可见输出索引，不能将未传入的输出按顺序补成一个已比较张量。

**检查结果**：对应表若有缺项，仍可继续分析其他可信配对，但结论只覆盖那些配对；缺少首异常调用的任何必要输入时，不能写“全部输入正常”。

## 统计筛查怎样读

原生 `dump_data=stats` 输出 CSV。对**已有同输入 golden 的张量**，先计算 golden 的同名统计，再与 CSV 比较。从已有 golden 计算统计不属于生成 golden。确认 CSV 行的 `Input/Output`、`Index`、`Shape`、`Data Type`、`Format` 一致后，再读取统计值。计数项精确核对；Max、Min、Avg、实际采到的 L2norm 使用明确的筛查阈值，不能根据官方推荐的相似度阈值推定。

| 统计现象 | 能说明什么 | 还不能说明什么、下一步 |
| --- | --- | --- |
| `Nan Count` 或正负 Inf 计数不同 | 两侧特殊值数量不同，应优先核验该观察点 | 无法确定位置；采集 tensor 核对特殊值坐标，并核对该调用输入 |
| Max/Min 超出筛查范围 | 值域出现差异，可能是一处大误差或整体尺度变化 | 无法仅凭值域判断根因；采集输出 tensor，分析误差分布 |
| Avg 不同、Max/Min 接近 | 整体均值偏移，可能涉及较多元素 | 不能判断每个元素的误差；按工程判据比对 tensor |
| 所有统计相同，包括 L2norm | 已比较的摘要未发现差异 | 排列、通道置换或正负误差抵消可保持这些统计；最终输出已超差就继续采 tensor |
| 当前调用的输入统计已有差异 | 异常可能从上游或本次输入传来 | 核对实际输入 tensor，再向上游找有参考的边界，不能直接把当前输出差异归为本调用引入 |
| golden 只有最终输出 | 最终输出统计可比较；中间 CSV 可记录特殊值现象 | 中间层没有参考，不能仅根据数值表现判定首个异常层 |

例如 golden `[1,2,3,4]` 与 actual `[4,3,2,1]` 的 Max、Min、Avg、NaN/Inf 计数和 L2norm 都相同，但四个位置逐元素不同。因此，stats 用于筛查候选，不能作为精度验收依据；未发现统计差异不等于 PASS。

遇到 CSV 统计列为 `NA`、缺列或数值无法可靠解析时，将该指标记为不可用。`dump_stats.py` 要求 Max、Min、Avg 和三类特殊值计数可用，L2norm 为可选项。若本次只要求采 Max/Min 等部分统计，直接整理这些列即可，无需为满足辅助脚本补采其他指标。

使用脚本时，只要必需指标不可用，该配对状态就是 `incomplete`，整批返回 2；已发现的差异仍保存在 `statistics` 中，对应张量仍列入 `candidates`。可选 L2norm 未采到时记入 `optional_unavailable`，不因此将配对标为未完成。

整数 Max/Min 只有在 CSV 保留完整十进制整数时才按整数精度筛查；指数或带小数的表示不能证明低位未舍入。整数筛查阈值按命令行十进制原值比较；Avg/L2norm 仍是浮点摘要。golden 含 NaN/Inf 时，脚本只比较特殊值计数，范围和范数保留不可用，有限值子集的统计不能代表完整张量统计。

已确认配对且需要批量整理时使用 [dump_stats.py](../scripts/dump_stats.py)；它读取 CSV 并计算已有 golden 的统计，不调用 msaccucmp 解析 CSV。`--atol/--rtol` 是**统计筛查阈值**，输出的 `candidate` 不是精度失败，`no_stat_difference` 也不是精度通过。示例 pairs JSON 中的路径相对 JSON 文件：

```json
[
  {
    "label": "request-a/node-a/output0",
    "actual": {"path": "dump/node-a.csv", "io": "output", "index": 0},
    "golden": {"path": "golden.npz", "key": "request-a/node-a/output0"}
  }
]
```

调用脚本时传入 `--pairs stats-pairs.json --atol <筛查绝对阈值> --rtol <筛查相对阈值> --out stats-result`。

一个 CSV 有多个同方向、同索引的候选行时，应先限定请求范围或补充调用标识，确保候选行可唯一确定。当前脚本对连续 ND、NCHW、NHWC、NCDHW 布局做有限支持；非 ND 布局必须在 golden 描述中声明 `"format": "NCHW"` 等实际布局，并确认轴顺序一致。其他格式先采 tensor 并进行受支持的布局转换。原生统计覆盖的元素范围与 golden 不一致时，例如包含额外填充元素，不能直接比较两侧摘要。

## 官方工具怎样选

先在执行端找到**配套 CANN** 的 `tools/operator_cmp/compare/msaccucmp.py`，查看 `convert --help` 和 `file_compare --help`。常规任务使用这两个子命令即可。参数定义与约束可查[官方 msaccucmp 命令实现](https://raw.githubusercontent.com/Ascend/msprobe/master/python/msprobe/msaccucmp/msaccucmp.py)；真实工具位置、参数和产品能力以当前安装为准。下面 `<cann-root>` 表示当前 CANN 根目录，其他尖括号内容根据任务实际信息替换。

1. **convert：原始 Dump → NPY。** `-d` 接受单个 raw、逗号分隔的 raw 列表或直接存放 raw 的叶目录，`-out` 指定新目录，`-t npy` 明确输出类型。目录不递归；返回 0 但没有目标 NPY 时仍是未完成。原始 Dump 有头信息和张量描述，不用 `numpy.fromfile` 当作裸数组读取。转换一个 raw 可得到多个输入和输出 NPY。
2. **file_compare：已配对 NPY → 官方指标。** `-m` 是实际张量，`-g` 是对应 golden，`-out` 是结果目录。golden 为 NPZ 时，先根据明确 key 导出 NPY；多对张量逐对比较，或用下文的辅助脚本批量调用。保留原始指标，按工程判据再判断超差和验收；此子命令不接收工程的 atol/rtol，也不负责输出异常元素坐标。

```text
python "<cann-root>/tools/operator_cmp/compare/msaccucmp.py" convert -d "<raw文件或叶目录>" -out "<转换目录>" -t npy
python "<cann-root>/tools/operator_cmp/compare/msaccucmp.py" file_compare -m "<actual.npy>" -g "<golden.npy>" -out "<结果目录>"
```

**只有实际布局需要时才做 Format 转换。** 不带 `-f` 的 `convert` 只解析文件；NC1HWC0、FRACTAL_NZ 等存储布局需要还原为 golden 的逻辑布局时，按当前工具支持添加 `-f <目标Format>`。目标 Format 和 shape 来自工程，不能任意 reshape 或试 shape 直到接近 golden。FRACTAL_NZ 还需 `-s <真实逻辑维度，以逗号分隔>`。`-o/-i` 互斥，指定输出/输入索引，只在带 `-f` 时有效；给目录转布局时这些索引和 `-s` 会被忽略，需要指定单个 raw 完成相应转换。

```text
python "<cann-root>/tools/operator_cmp/compare/msaccucmp.py" convert -d "<需要转布局的raw文件>" -out "<另一转换目录>" -f <目标Format> -t npy
```

BF16 raw 解析成 NPY 时可能以 FP32 保存，报告保留原始 BF16 类型及转换说明。`convert` 应产出目标 NPY；`file_compare` 当前保存 `file_result_{timestamp}.txt` 汇总指标，以执行端实际输出为准。核对数据内容、来源和结果对应关系，成功退出但结果缺失或不匹配时保留未完成状态。

## 官方指标怎样解释

[官方比对结果说明](https://msprobe.readthedocs.io/zh-cn/latest/zh/user_guide/accuracy_compare/offline_data_compare_instruct.html)列出了常见指标的含义。该页展示 msprobe 的整网入口，指标用于解释数值形态；具体命令仍使用上面的配套 msaccucmp。报告采用实际输出的字段；整网比对支持的指标不一定全部适用于 `file_compare`。

先核对执行端 `msaccucmp` 计算指标时采用的 dtype。[官方指标计算实现](https://raw.githubusercontent.com/Ascend/msprobe/master/python/msprobe/msaccucmp/algorithm_manager/algorithm_manager.py)包含在计算前将非 bool 数据转成 `float32` 的处理；对较大的 `int64/uint64` 等无法用该类型精确表示的值，MaxAbsoluteError、均值、RMSE、余弦等官方摘要可能丢失低位，显示零误差也不能证明整数逐元素相等。保留官方原始结果；整数的工程 PASS/FAIL、最大绝对误差和坐标须从保留整数值的转换 NPY，用工程 evaluator 或 `summarize.py` 的精确 `details` 核对。

| 指标 | 看什么 | 不能代替什么 |
| --- | --- | --- |
| CosineSimilarity | 两份张量整体方向是否接近；越接近 1 通常越相似 | 一个或少数元素的大误差可能被大量正常元素稀释；高余弦不代表逐点满足 atol/rtol |
| RelativeEuclideanDistance | 相对整体距离；越接近 0 通常越相近 | 不能给出异常坐标，也不是项目的逐元素容差 |
| KullbackLeiblerDivergence | 工具实际给出时，可观察分布差异 | 对零值、非有限值或不适合分布解释的张量要谨慎；`file_compare` 未必提供该项 |
| MeanAbsoluteError、RootMeanSquareError | 整体误差幅度及大误差对均方根的影响 | 不能仅凭均值推断超差元素数量或连续异常区域 |
| MaxAbsoluteError、MaxRelativeError 等 | 工具计算精度下的最大差异摘要，适合发现局部尖峰 | 大整数可能在指标计算前舍入；不能代替原始值、精确整数误差和工程判据 |

官方页面给的推荐数值不构成本工程的默认验收线。若项目判据是逐元素 `|actual-golden| <= atol + rtol × |golden|`，需要对规定有效区的每个元素、允许失配比例和特殊值规则一起判断；项目实际 evaluator 若还有其他条件，按其完整规则。高余弦、低平均误差或官方摘要中的“相似”都不能覆盖已证实的局部超差。

比对结果出现 `NaN`、`Inf`、`*` 或未匹配时，先区分三种情况：数据包含特殊值、零向量或零分母导致指标无定义，以及节点或格式无法配对。查看同一行的其他指标、原始张量和映射；不能将 `NaN` 一律解释成输出含 NaN，更不能直接写 PASS/FAIL。shape、布局、节点或输入索引无法对应时先解决配对，单个较高的相似度指标不足以证明配对正确。

## 从差异走到“最早可观察边界”

沿模型依赖关系或 Host 调用顺序，检查**有参考且可比**的观察点。输出超差时，列出该调用全部可见输入及非张量属性，逐项核对实际值与对应参考：

- 输入已超差：当前输出可以受到上游差异影响，继续核验上游，缩小异常范围；不能将当前调用写成独立首次异常。
- 全部必要输入已证实一致、输出按工程判据超差：可以确认该调用的输入输出边界存在偏差；只有检查过相关可比上游，才能称为已核验路径上的“最早可观察偏差”。若 A 超差、B 回到容差内、C 再次超差，C 的输入正常不影响 A 已出现异常这一事实，也不足以证明 C 是全图唯一的首个异常点。
- 输入缺少参考、未采集常量/权重/偏置，或中间点没有 golden：报告已观察到的输出偏差与缺失信息，更早的异常边界保持未确定。
- 后续节点暂时回到容差内：该结论仅适用于当前观察点，上游已观察到的偏差仍需保留；最终又超差时按实际依赖关系继续解释传播。

## 辅助脚本的输入输出

辅助脚本依赖执行端 Python 与 NumPy，`summarize.py` 还需同一 Python 能运行配套 `msaccucmp`；优先使用工程已有环境。它们只接受已经确认的配对，不自动识别业务请求、恢复布局或推断节点关系。

| 脚本 | 输入 | 输出到新的 `--out` 目录 | 退出码 |
| --- | --- | --- | --- |
| [dump_stats.py](../scripts/dump_stats.py) | stats pairs JSON、原生 CSV、golden、明确的筛查阈值 | `summary.json`、`report.md`，含指标差值、候选和缺项 | 0：未发现统计差异；1：有候选且无 `incomplete` 配对；2：存在 `incomplete` 配对或输入错误。0 不代表精度通过 |
| [summarize.py](../scripts/summarize.py) | tensor pairs JSON、官方转换 NPY、golden、工程判据；可选已有 stats 汇总 | `summary.json`、`report.md`、导出的 `golden-<序号>.npy`、`official-<序号>/` 官方结果与日志；有逐元素判据时再保存 `mismatch-<序号>.npy` | 0：所列比较项通过脚本支持的检查；1：存在失配；2：比较未完成、输入/输出错误或判据/类型未判定。以逐项结果为准 |

输出目录需不存在或为空。`mismatch-<序号>.npy` 是与 actual 同 shape 的布尔数组，`True` 表示按本次逐元素判据超差；路径记录在 `details.mismatch_mask`，相对汇总输出目录。它覆盖整个张量，与 Top-K 数量无关；没有逐元素判据时不生成掩码，保留未判定状态。

`summarize.py` 每开始、完成一对都会先原子保存辅助报告，再原子更新 `summary.json`；汇总文件最后写入，确保报告保存成功后再记录完成状态。`pending` 表示尚未执行，`running` 表示已开始但未确认完成，`incomplete` 表示该项未完成；均不能算通过。`execution_complete=true` 仅表示整批执行结束，失败项仍须逐项处理。中断后以保留的 `summary.json` 为准：若进程已结束而某项仍是 `running`，该项按未完成处理，不从目录中存在官方文件推断成功。

`--timeout` 限制的是单对官方比较，不是整批用时；按当前工具调用时限拆分 pairs。脚本不自动续跑非空目录。中断后保留原目录，将未完成项整理为新的 pairs，在新目录执行；已完成项只有在 actual、golden、判据和官方工具身份保持一致时才引用复用，无需仅为汇总报告而重新比较全部数据。报告表格中的 shape、元素数、非有限值数量和误差直接来自各项结果，正文沿用这些数值并注明适用范围。

`summarize.py` 用于批量调用官方 `file_compare` 并补充**整个同 shape 张量**的逐元素 `exact` 或 `atol/rtol` 结果，按配置可处理同位置 NaN。该脚本的 `numeric_state` 是数值检查，`dtype_contract_state` 是转换 NPY 的类型检查；`precision_verified=true` 表示所列检查已作出判断，结果仍可能是失配，不表示精度通过。只有工程完整判据已全部核验，才能给工程最终 PASS/FAIL。

`criterion.equal_nan` 只接受 JSON 布尔值 `true`/`false`；字符串 `"false"` 或数字不能代替布尔值，脚本会将该项标为 `incomplete`。仅当工程特殊值规则明确允许同位置 NaN 相等时设为 `true`。

若工程还规定有效区、允许超差比例、独立最大误差上限、跨输出组合条件或其他特殊值规则，脚本的全张量计数与状态仅供诊断；必须调用工程 evaluator 或按已声明的完整规则另行判断。相关核验尚未完成时，将最终验收标为 `unjudged`，不能将脚本的 `mismatch` 直接写成项目 FAIL，也不能将 `pass` 当成其他条件已通过。合法有效区须来自任务原有规则，并保留其掩码和原始坐标映射；不得通过事后裁剪数据减少超差计数。

整数张量的逐元素容差按 pairs JSON 中的阈值和整数值精确判断；带小数或指数的阈值在报告 JSON 中以字符串保留原值。报告将官方 `metrics` 与精确整数 `details` 分列：最大绝对误差及 Top-K 绝对误差来自 `details`，相对误差仍是浮点摘要，极大整数附近的末位仅供观察。官方 `file_compare` 不接收 pairs 中的 `atol/rtol`，因此同一对 NPY 换容差后官方指标可完全相同；不能拿它决定整数 PASS/FAIL。`exact=true` 与 `atol/rtol` 只能选一种，避免同一对数据有两种判据。以下示例仅展示脚本支持的逐元素判据：

```json
[
  {
    "label": "request-a/node-a/output0",
    "actual": {"path": "converted/node-a.output.0.npy"},
    "golden": {"path": "golden.npz", "key": "request-a/node-a/output0"},
    "expected_actual_dtype": "float32",
    "criterion": {"atol": 0.00001, "rtol": 0.00001}
  }
]
```

示例阈值和期望 dtype 只是 JSON 格式展示，实际值来自项目。路径相对 pairs 文件。没有逐元素判据时给误差统计并标 `unjudged`；形状、布局或参考身份不匹配时保留未完成项，不能跳过失败项宣称全部通过。脚本可以检查 shape 和受支持的 dtype，布局与参考身份须由调用者事先确认。两侧是可比、最高为 float64 的浮点张量时，脚本保留原文件并升至 float64 做数值诊断，例如 FP16 输出与 FP32 golden；整数与浮点混合则标为 `incomplete`。

`expected_actual_dtype` 用于检查转换 NPY 的类型，取值来自工程元数据和转换规则。未提供时，类型检查为 `unjudged`，数值通过的综合状态也为 `unjudged`；数值失配仍记录差异。期望类型与实际不一致时，类型检查为 `mismatch`。数值判据或类型检查任一未判定时，`precision_verified=false` 且退出码为 2，已发现的另一类失配仍保留。这些状态都不能代替未核验的工程条件。

若官方转换改变了类型表示，`expected_actual_dtype` 应填经元数据和转换规则确认的 NPY 类型，例如 BF16 raw 对应 FP32 NPY 时填 `float32`；原始张量是否满足工程要求的 BF16 类型须另行核对，不能用 NPY 类型检查代替。

调用 [summarize.py](../scripts/summarize.py) 时传入 `--pairs tensor-pairs.json --tool "<cann-root>/tools/operator_cmp/compare/msaccucmp.py" --out tensor-result`。

已有原生 stats 汇总时可追加 `--stats stats-result/summary.json`，没有则省略。省略 `--tool` 时，脚本先查 `MSACCUCMP_PATH`，再查 `ASCEND_HOME_PATH` 或 `ASCEND_TOOLKIT_HOME` 下的配套路径；确认找到的是本次执行端使用的工具。少量张量可直接使用官方命令及工程 evaluator，无需为使用辅助脚本改写工程判据。

## 成套模型数据的可选比对

模型 Dump 与 golden 已成套、节点映射可确认，且需要整体结果表时，再使用 `msaccucmp.py compare`。`-m` 指向工具可识别、直接存放本轮 NPU 算子 Dump 的目录，`-g` 指向对应的 golden 目录。经 ATC 融合时，按配套工具要求用 `-f` 提供模型 JSON；`-cf` 用于 golden 是关闭融合模型的 NPU Dump 且需要其模型 JSON 的场景，不适用于替代普通 CPU/GPU golden 的映射。

```text
python "<cann-root>/tools/operator_cmp/compare/msaccucmp.py" compare -m "<NPU模型Dump目录>" -g "<成套golden目录>" -f "<ATC模型JSON>" -out "<结果目录>"
```

只有零散的配对 NPY 时，继续用 `file_compare`。范围、单算子详情和 Advisor 只在确有相应任务时查配套工具帮助；这些比对选项作用于已采数据，采集量仍由 `acl.json` 控制。`compare -f` 是模型 JSON，`convert -f` 是目标布局，两者不能混用。

[msprobe 离线数据比对文档](https://msprobe.readthedocs.io/zh-cn/latest/zh/user_guide/accuracy_compare/offline_data_compare_instruct.html)还介绍了封装入口 `msprobe compare -m offline_data -tp ... -gp ... -fr ... -o ...`。该入口的 `-tp/-gp/-fr/-o` 与 msaccucmp 的 `-m/-g/-f/-out` 属于不同参数体系，不可混用。已有配套工具可用时，无需为采用该入口另行安装工具。

## 报告骨架

只保留本次用到的小节；用户已有格式就沿用。正文数字从实际保存的结果读取，不手写与数据矛盾的汇总。报告应清楚呈现以下内容：

| 要回答的问题 | 报告中的内容 |
| --- | --- |
| 配置是否达成采集目标 | 目标、所选字段及理由、实际配置入口和运行命令、本轮数据目录及采集覆盖 |
| 哪些统计不同 | 每个可比张量的实际/golden Max、Min、Avg、特殊值计数、实际可用的 L2norm，以及差值和筛查规则 |
| 哪个张量、哪些位置超差 | 请求、模型/调用、方向与索引、shape、dtype/布局、工程判据、超差数量及比例、最大误差和原始坐标及双方值 |
| 能把问题缩小到哪里 | 已比较的输入输出边界、下一步补采范围，或缺少 golden、必要输入及映射而无法确认的部分 |

异常区域来自完整超差掩码，Top-K 只展示样本。只交付采集时，报告配置、命令、文件位置与归属即可。

```markdown
# Runtime Dump 数据定位报告

## 结论

- 已证实：哪些请求/输出超差；最早可观察的调用或张量边界及其证据。
- 未证实：缺少哪些输入或中间 golden；是否存在更早偏差仍未知。

## 采集与对应

- 执行端版本、工程入口、同输入身份、模型/调用、配置与采集目录。
- 原生 stats 是否真实采到；raw、转换 NPY、golden 的方向/索引/shape/dtype/布局对应。
- 工程判据来源和实际 evaluator：有效区及原始坐标映射、容差公式、允许超差比例、最大误差上限、特殊值和多输出组合规则；缺项注明。

## 差异证据

- stats 的 Max/Min/Avg、NaN/正负 Inf 计数及实际支持的 L2norm 与 golden 的差值。
- 官方比对文件、辅助逐元素统计和工程 evaluator 的最终结果分别列出；按实际有效区给超差数量和比例、最大误差、Top-K 原始坐标及双方值、非有限值类别。
- 首异常边界全部可见输入的核对状态；后续传播与未覆盖点。
```

异常区域必须由**完整超差掩码**核对，说明坐标从 0 还是 1 开始、区间端点是否包含；Top-K 只是少量位置，不能据它猜整片区域。只有最终输出 golden 时，报告最终差异和中间采集现象，不声称第一个错误中间算子。原始 Dump、官方结果及报告尽量留在任务独立目录，不在正文粘贴大量张量或敏感输入。
