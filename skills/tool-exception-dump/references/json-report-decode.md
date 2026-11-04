# info.json 结构化报告解码（msaicerr -p 的 AI 亲和产物）

> **使用时机**：Phase 2 执行 `msaicerr -p` 之后、逐步骤判读 msaicerr 结果之前必读本文件。info.json 是与 info.txt 同源、同目录落盘的结构化报告，供 AI 按字段级取值判读；本文件回答三个问题：info.json 何时产出、每个字段的取值与空值语义、缺失时如何回退 info.txt。
>
> **引用约定**：本文 path:line 引用相对于 CANN 社区仓 cann/oam-tools 根目录。行号为源文件真实行号，可疑时按行号回读源码核实。文中 json_report_builder.py、aic_error_info.py、aicore_error_parser.py 均指 `oam-tools/src/msaicerr/ms_interface/` 下的同名文件，msaicerr.py 指 `oam-tools/src/msaicerr/msaicerr.py`，test_json_report_builder_ut.py 指 `oam-tools/test/ut/msaicerr/testcase/test_json_report_builder_ut.py`。

---

## 一、产物与版本约束

**仅 `-p` 模式产出**：`main()` 按参数分派到三条入口——`-d`（args.data）走 `convert_dump_data`、`-p`（args.report_path）走 `analyse_report_path`、`-e`（args.env）走 `test_env`（msaicerr.py:401-406）；其中只有 `analyse_report_path` 内的 `AicoreErrorParser(...).parse()` 会调用写盘（msaicerr.py:151-152）。`-d` 与 `-e` 不产出 info.json。

**版本边界（待上游确认）**：**info.json 是 CANN 9.3.0 之后版本才出现的产物，9.3.0 及之前仅有 info.txt**——此主张标注为用户口径（2026-11-03），仓内无版本标记可引，待上游文档确认，不写成有码锚事实。判读时不得宣称 CANN ≤9.3.0 场景 info.json"应存在而缺失"。

**旁路可失败**：`write_json_report` 整个函数被 try/except 包裹，任何异常仅告警并删除半截文件、不影响主流程与退出码（json_report_builder.py:781-799）。写盘前先序列化成完整字符串再落盘，序列化失败不产生半截文件（json_report_builder.py:785-789）。UT 锚定：无半截文件 test_json_report_builder_ut.py:464-482，写失败旁路 test_json_report_builder_ut.py:628-647。

**两个写盘点**：

| 路径 | 形态 | 触发条件 | 源码锚点 |
| --- | --- | --- | --- |
| 成功路径 | `<out>/info_{ts}/aicerror_0_{errts}/info.json` | 收集成功且提取到 AI Core Error 信息，`analysis_result="success"` | aicore_error_parser.py:2119-2123；目录拼接 msaicerr.py:135-138 + aicore_error_parser.py:2000-2001 |
| 退出码 8 路径 | `<out>/info_{ts}/aicerror/info.json` | 收集失败或日志无法提取信息，`analysis_result="failed"` | aicore_error_parser.py:1988-1995 |

退出码 8 路径的子目录是 `aicerror/`（**无时间戳后缀**），与成功路径的 `aicerror_0_{errts}` 形态不同——按 glob `aicerror_0_*` 找结果会漏掉它。

**终端提示**：写盘成功后打印 `Analysis info (json) is saved in <路径>`（json_report_builder.py:790），与 info.txt 的 `Analysis info is saved in <路径>`（aicore_error_parser.py:1496）成对出现。据此可同时定位两份结果文件。

## 二、顶层 schema 总表

`build_report` 返回的顶层 dict 固定 14 个键（json_report_builder.py:751-772；UT 锚定 test_json_report_builder_ut.py:598-619）：

| 键 | 取值/类型 | 说明 |
| --- | --- | --- |
| report_version | `"1.0"` | 报告版本（json_report_builder.py:41） |
| report_type | `"aicore_error_analysis"` | 报告类型（json_report_builder.py:42） |
| analysis_result | `"success"` / `"failed"` | 采集是否成功，**判读只认此键**（见「三、空报告语义」） |
| collection_dir | 字符串 | 路径相对化基准目录，即 `<out>/info_{ts}/collection`（json_report_builder.py:755） |
| generated_at | 字符串 | 报告生成时间戳（json_report_builder.py:761） |
| root_cause | 对象 | `{summary, evidence, action_items}`（见「十一」） |
| basic_info | 对象 | 基本信息段（error_time/device_id/core_id/task_id/stream_id/node_name/kernel_name/kernel_file/json_file/cce_file/graph_file/rts_block_dim/driver_aicore_num，json_report_builder.py:292-313） |
| aic_error | 对象 | `{groups, extend_info}`（见「七」） |
| instructions | 对象 | `{original, corrected, notice}`（见「六」） |
| args | 对象 | `{before_execute, after_execute}`（见「八」） |
| dump_info | 对象 | `{exception_dump_file, tiling_data, files}`（见「九」） |
| single_op_test | 对象 | `{result, execute_info, memory_status, log_path}`（见「十」） |
| env_check | 对象 | `{result, detail}`（见「十二」） |
| op_in_graph | 恒 `null` | 占位字段，当前未实现（json_report_builder.py:770） |

basic_info 的空值口径：文本类字段取不到输出 `not_collected`，与文件/数值字段一致，避免同一字典出现 `""` 与 `not_collected` 两种空值语义（json_report_builder.py:293-294）。

## 三、空报告语义

**判读只认 `analysis_result`**：当 `analysis_result == "failed"` 时，报告由一个空的 `AicErrorInfo()` 构建（aicore_error_parser.py:1990-1993），其余字段全部为默认空值——包括 `single_op_test.result` 会是 `"Not Run"`、`env_check.result` 是 `"not_collected"`。**这些都不是真实采集结果**：单算子复现根本没跑、环境自检根本没执行，只是空对象默认值渲染出来的样子。

判读纪律：`analysis_result == "failed"` 即按"输入信息不可用"处理（回到三要素排查与日志时间窗核对），**忽略其余所有字段**，不得把 `single_op_test.result: "Not Run"` 当成"用例未运行"的复现结论、也不得把 `env_check.result: "not_collected"` 当成环境检查失败或未采集的证据。

对应源码链：`collect_succ=False` → `analysis_result` 取 `"failed"`（json_report_builder.py:759）；`single_op_test_result` 默认 `NOT_RUN` 且 `single_op_attempted` 默认 `False` → 四态取 `"Not Run"`（json_report_builder.py:566-567）；`env_check_attempted` 默认 `False` 且 `collect_succ=False` → `not_collected` 且 `detail` 留空（json_report_builder.py:731-742，UT 锚定 test_json_report_builder_ut.py:437-442）。

## 四、空值三义

info.json 只有三种合法空值，语义互不混用（UT 锚定 test_json_report_builder_ut.py:63-105、87-92、95-98）：

| 空值 | 含义 | 典型场景 |
| --- | --- | --- |
| `not_collected` | 采集未取到 | 数值哨兵 `-1`（json_report_builder.py:45、163-167）；文件路径不存在（170-174）；args 列表为空（484-485）；寄存器位段解析失败（250-254） |
| `null` | 场景不适用 | `instructions.corrected`（无 PC 修正）、`instructions.notice`（无修正提示）、`basic_info.graph_file`（未命中图算子）、`op_in_graph`（占位）、`dump_info.exception_dump_file`（无 dump 来源） |
| `""` | 采集到但为空 | `single_op_test.memory_status`（`single_op_mem_monitor` 为空串时，json_report_builder.py:579） |

全报告禁止出现 `N/A`、`None`、`-1` 字面（UT 全报告扫描断言 test_json_report_builder_ut.py:119-127）。

**细化**：`corrected.src_locations` 在 `corrected` 存在但解析不出 outerSrc/innerSrc 时取 `not_collected` 而非 `null`——因为 corrected 对象已存在、字段确有含义只是没取到，与"整个 corrected 不适用（null）"是两个层级（json_report_builder.py:352-353，`_parse_src_locations(...) or NOT_COLLECTED`）。

## 五、路径相对化

所有文件路径相对 `collection_dir`（即 `<out>/info_{ts}/collection`）输出，正斜杠分隔，集合目录外的路径以 `../` 前缀表达（UT 锚定 test_json_report_builder_ut.py:551-592）：

- `_to_rel` 用 `os.path.relpath` 计算相对路径并 `replace(os.sep, "/")`，跨盘符等失败时退回原路径（json_report_builder.py:177-186）。
- 集合内文件：`collection/compile/kernel.o` → `compile/kernel.o`（UT :551-561）；`collection/dump/xxx` → `dump/xxx`（UT :575-585）。
- 集合外文件：`info_{ts}/aicerror_0/log.txt` → 以 `../` 开头（UT :564-572）。
- 路径不存在或未取到：走 `not_collected`（`_path_or_not_collected`，json_report_builder.py:170-174，UT :588-592）。

## 六、instructions

`instructions` 由三键组成（json_report_builder.py:338-363；UT 锚定 test_json_report_builder_ut.py:702-747、778-787）：

- `original`：`{start_pc, current_pc, likely_error_line}`——原始 PC 与反编译得到的报错行（来源 `info.instr`，json_report_builder.py:340-344）。
- `corrected`：`{start_pc, current_pc, likely_error_line, src_locations}` 或 `null`——PC 修正可用时输出（来源 `info.corrected_pc`/`info.corrected_instr`）；无修正时整体为 `null`（不适用）（json_report_builder.py:347-358）。
- `notice`：修正提示文本或 `null`——与 original/corrected 平级，文案即 `CORRECTED_PC_NOTICE`（aic_error_info.py:25-28）。

`src_locations` 为 `[outerSrc, innerSrc]` 列表（按文本顺序 outerSrc 在前、innerSrc 在后，json_report_builder.py:331-335），取不到时为 `not_collected`（见「四」）。

**`likely_error_line` 是十进制整数**：由第③段 `Error occurred most likely at line:` 后跟的十六进制偏移经 `int(..., 16)` 换算（正则按 `0x[0-9a-fA-F]+` 捕获，json_report_builder.py:316-328）。换算关系：info.txt 里的 `0x5e8`（十六进制偏移）→ JSON 字段为十进制 `1512`（UT :704-710 断言 `== 0x5E8`）。判读时若需与 info.txt 第③段值对照，注意两者进制不同、需相互换算，不要直接把 JSON 的十进制 int 与 info.txt 的十六进制串当同一形态比较。

## 七、aic_error

`aic_error` 为 `{groups, extend_info}`（json_report_builder.py:259-289）：

- `groups`：数组，每项 `{error_code, cores_aic, cores_aiv, registers}`。`error_code` 是**字符串原样**（`str(code)`，json_report_builder.py:276、283）；`cores_aic`/`cores_aiv` 分别是 AIC/AIV 核号列表；`registers` 见下。
- `extend_info`：plog 的 extra_info 原文（`info.extra_info`，json_report_builder.py:289）。
- 无分组信息时（`aic_error_groups` 为空）由单条 `aic_error_info` 合成一个 group（json_report_builder.py:260-268）；多组错误时输出多个 group（UT :675-699）。

`registers` 仅 910B（Stars）非空，键为模块名（vec/mte/ifu/ccu/cube/biu），值为 `{raw_value, bits, value, meaning, approximate}`；950（David）恒为 `{}`（json_report_builder.py:277-278、227-256）。`bits` 形如 `"[28:16]"`，`meaning` 为地址段含义（如 `"VEC Error Address [17:5]"`），`approximate` 为补零近似地址（十六进制，仅供参考）。**无 `chip_type`、无 `error_bits` 字段**（UT 断言 test_json_report_builder_ut.py:654-656、670-672）。

## 八、args

`args` 为 `{before_execute, after_execute}`，各自为列表或 `not_collected`（列表为空时取 `not_collected`，json_report_builder.py:473-488）。每个条目六键（json_report_builder.py:391-427）：

| 键 | 取值/类型 |
| --- | --- |
| arg_index | 整数（全局 args 序号） |
| arg_type | `input`/`output`/`workspace`/`tiling_data`/`shape`/`other`（枚举见下） |
| address | `0x` 前缀十六进制**小写**字符串（`_hex_addr`，json_report_builder.py:189-194） |
| dtype | 字符串或 `null`（workspace/tiling_data/shape 不适用为 `null`） |
| shape | `"[8, 2048]"` 单行字符串或 `null`（`_shape_inline`，json_report_builder.py:384-388） |
| size_bytes | 整数或 `null` |

`arg_type` 枚举语义（json_report_builder.py:395-411）：`input`/`output`/`workspace`/`tiling_data` 由 plog IDEDD 标记（`args_type_marks`）；`shape` 为 ffts 尾部 c2c 追加参数（`ffts_addrs_num` 兜底，json_report_builder.py:400-401）；`other` 为降级（含 plog 标记的 `normal` 在 json 缺失时降级为 `other`，json_report_builder.py:396-399）。

**106 判定对照**：info.txt 第④段 args before/after 不一致指向 106（args 被踩）；JSON 侧等价于逐 `arg_index` 对比 `before_execute` 与 `after_execute` 两列表（同 index 的 address/dtype/shape 是否一致）。注意 JSON 是语义化后的列表（含 arg_type 与张量元数据），info.txt 第④段是原始地址串，两者同源但形态不同，对照以 JSON 字段级对比为准（UT 锚定 test_json_report_builder_ut.py:133-278）。

## 九、dump_info

`dump_info` 为 `{exception_dump_file, tiling_data, files}`（json_report_builder.py:531-559）：

- `exception_dump_file`：报错 dump 采集文件路径（相对路径）或 `not_collected`（有 `dump_file` 但文件不存在）/`null`（无 dump 来源）（json_report_builder.py:545-556）。
- `tiling_data`：tiling 数据文件路径（相对路径）或 `not_collected`（json_report_builder.py:557）。
- `files`：数组，每项 `{file, arg_index, data_stats}`。`file` 为张量 dump 文件路径；`arg_index` 为**全局 args 序号**（与 `args[].arg_index` 同一语义，经 `_dump_arg_index` 把 dump 内部序号换算为全局序号，json_report_builder.py:491-520）；`data_stats` 为该张量的统计信息列表。`files` 不输出 shape/dtype 字段（UT 断言 :208-210）。

`arg_index` 全局语义要点：dump json 带 arg_index 时原样返回；output 无该字段时按"内部序号 + num_inputs"换算，避免与 input 撞号（UT 锚定 test_json_report_builder_ut.py:213-245）。

## 十、single_op_test 四态与三态映射

`single_op_test` 为 `{result, execute_info, memory_status, log_path}`（json_report_builder.py:562-583；UT 锚定 test_json_report_builder_ut.py:750-776）。

**四态表**（`result` 取值由 `RetCode` + `single_op_attempted` 决定，json_report_builder.py:566-573）：

| JSON `result` | 判定字段 | 含义 |
| --- | --- | --- |
| `Aicore Error` | `single_op_test_result is FAILED` | 复现成功（用例复现出 AI Core error） |
| `Execute Success` | `single_op_test_result is SUCCESS` | 未复现（用例执行完但没复现，不代表算子无问题） |
| `Execute Failed` | `NOT_RUN` 且 `single_op_attempted=True` | 执行失败（跑了但失败，如 tiling/config 构造失败） |
| `Not Run` | `NOT_RUN` 且 `single_op_attempted=False` | 未执行（用例根本没跑，如 SK 场景跳过） |

**四态到三态判读框架的映射**（三态即 info.txt 第⑥段 `SINGLE_OP_RESULT_MSG` 三条文案，aic_error_info.py:32-39）：

| JSON 四态 | 三态判读 | RetCode |
| --- | --- | --- |
| `Aicore Error` | 复现成功 = `Aicore Error` | FAILED |
| `Execute Success` | 未复现 = `Execute Success` | SUCCESS |
| `Execute Failed` | 未运行（NOT_RUN） | NOT_RUN |
| `Not Run` | 未运行（NOT_RUN） | NOT_RUN |

即：**`Execute Failed` 与 `Not Run` 都归"未运行"（NOT_RUN）**——两者共用 `SINGLE_OP_RESULT_MSG[NOT_RUN]` 同一条文案（"The single-operator test case was not executed..."），复现成功归 `Aicore Error`、未复现归 `Execute Success`。

其余三键：`execute_info` 与 info.txt 第⑥段文案**同源**（`SINGLE_OP_RESULT_MSG.get(single_op_test_result, not_collected)`，json_report_builder.py:576-578）；`memory_status` 为越界探测结论串，采集到但为空取 `""`（json_report_builder.py:579）；`log_path` 为单算子日志路径或 `not_collected`（json_report_builder.py:580-582）。

## 十一、root_cause 12 分支与返回码链顺序差异

`root_cause` 为 `{summary, evidence, action_items}`（json_report_builder.py:600-727）：

- `summary` **双来源**：错误位字典 `desc` 优先（`_error_code_desc`，json_report_builder.py:204-224），取不到（`not_collected`）时退回结论分支文本（`info.get_conclusion()`，json_report_builder.py:602-604）。
- `evidence`：数组，每项 `{field, value, interpretation}`，`field` 是 **info.json 内字段路径**（如 `aic_error.groups[0].error_code`、`single_op_test.result`）。
- `action_items`：数组，每项 `{action, target(可选), purpose}`，字段序固定 **action → target → purpose**（json_report_builder.py:715-725）。

**12 分支优先序全表**（`match_conclusion_branch`，aic_error_info.py:275-311；UT 全分支锚定 test_json_report_builder_ut.py:508-545）：

| 序 | 分支键 | 触发条件（源码转述） |
| --- | --- | --- |
| 1 | `atomic_clean_missing` | `atomic_clean_check` 为假：图中算子前未插入 memset/atomic_clean |
| 2 | `flag_mismatch` | `flag_check` 非空：set_flag/wait_flag 未配对 |
| 3 | `core_num_insufficient` | `rts_block_dim > driver_aicore_num * 2`（两者均非哨兵 -1） |
| 4 | `pc_zero` | `current_pc == "0x0"` |
| 5 | `atomic_overflow` | `atomic_add_err` 为真 |
| 6 | `data_invalid` | `dump_info` 含 `"data invalid"` 子串 |
| 7 | `single_op_reproduced` | `single_op_test_result is FAILED` |
| 8 | `args_mismatch` | `check_args_result` 为假：args 执行前后不一致 |
| 9 | `addr_invalid` | `addr_valid` 或 `data_dump_result` 为假 |
| 10 | `env_error` | `env_available` 为假 |
| 11 | `single_op_not_run` | `single_op_attempted` 且 `NOT_RUN` |
| 12 | `single_op_not_reproduced` | `single_op_test_result is SUCCESS` |
| — | `""` 兜底 | 以上全不命中，`get_conclusion()` 走内部错误/维测信息不足兜底 |

**与 get_return_code 返回码链（105→107→104→106→102→101→103→0，aicore_error_parser.py:2145-2162）的顺序差异**——两链同一套判定字段但排序不同，人工读文案（info.txt 首段结论文本 / JSON `root_cause.summary`）、脚本读返回码时预期这个差异：

1. **顺序不同**：结论文案链按上表 1-12 排序；返回码链按 105→107→104→106→102→101→103→0 排序。例：`args_mismatch` 在文案链排第 8，在返回码链对应 106 排第 4；`addr_invalid` 在文案链排第 9，在返回码链落在 102（落盘失败）/104（data invalid）方向。
2. **文案链独有分支**：`flag_mismatch`、`core_num_insufficient`、`pc_zero` 三个分支**没有专属返回码**——命中时只进结论文案，返回码落到后续分支或 0，依赖返回码告警会漏掉它们。
3. **多态归并**：`single_op_not_run` 与 `single_op_reproduced` 在返回码层都归 101（NOT_RUN 同样命中 101）；`single_op_not_reproduced` 落到 0 兜底。

## 十二、env_check

`env_check` 为 `{result, detail}`，`result` 三态（json_report_builder.py:730-748；UT 锚定 test_json_report_builder_ut.py:429-442）：

| result | 触发条件 | detail |
| --- | --- | --- |
| `success` | 环境自检已执行且 `env_available` 为真 | `""` |
| `failed` | 环境自检已执行且 `env_available` 为假 | `"Failed to execute the built-in sample operator. Check the environment."` |
| `not_collected` | 未真正执行 Step 10（异常跳过，或采集失败路径根本没走到 Step 10） | 采集失败时留空 `""`；异常跳过时给 skip 提示 |

判读：`not_collected` 表示环境自检未执行，不表示环境有问题；`failed` 才是自检失败（对应 103 硬件错误方向的证据）。

## 十三、字段↔步骤↔info.txt 段位映射总表

11 个读取步骤的判读各对应 info.json 字段，字段名以 json_report_builder.py 键名为准（与 todo 2 的字段映射清单一致），右列补 info.txt 段位对照：

| 步骤 | info.json 字段 | info.txt 段位 |
| --- | --- | --- |
| P2.3 报错行定位 | `instructions.original` / `instructions.corrected` / `instructions.notice` | 第③段 Operator Error Line Number |
| P2.4/P2.5 手动符号化/反汇编输入 | `instructions.corrected.likely_error_line` + `basic_info.kernel_file` | 第③段 Corrected Info 偏移 + 第①段 kernel file |
| P2.6 dump 失败分类 | `root_cause` 分支（`addr_invalid`/`data_invalid`）+ `dump_info` | 第⑤段 Operator Dump File Parsing |
| P3.1 芯片形态判定 | `aic_error.groups[].error_code` / `registers` | 第②段 AI Core DFX Register |
| P3.3 六模块子域 | `aic_error.groups[].registers` 模块对象（raw_value/bits/value/meaning/approximate） | 第②段各 `*_ERR_INFO` 子寄存器串 |
| P3.4 特殊场景 2（args 改写） | `args.before_execute` / `args.after_execute` | 第④段 Operator Input/Output Memory |
| P3.5 AI Core 数量与 blockDim | `basic_info.rts_block_dim` / `basic_info.driver_aicore_num` | 第①段 Basic information |
| P4.1 自动复现结果 | `single_op_test` 四态 + `execute_info` + `memory_status` + `log_path` + `dump_info.files[].data_stats` | 第⑥段 Execution Result of the Single-Operator Test Case + 第⑤段 |
| P4.2 三态与越界判读 | `single_op_test`（result/memory_status）+ `root_cause` | 第⑥段 |
| P5.1 四证据链汇聚 | `root_cause.evidence`（field/value/interpretation） | 首段结论文本 + 六节 |
| P5.2 结论码优先级链 | `root_cause` 分支 ↔ get_return_code 返回码（顺序差异见「十一」） | 首段结论文本 |
| P5.3 修复建议映射 | `root_cause.summary` / `root_cause.evidence` / `root_cause.action_items` | 首段结论文本 |

info.txt 六段与 JSON 顶层键的粗对应：第①段→`basic_info`；第②段→`aic_error`；第③段→`instructions`；第④段→`args`；第⑤段→`dump_info`；第⑥段→`single_op_test`。结论文本（首段）→ `root_cause.summary`。

## 十四、回退规则

**缺失判定用文件存在性，不做版本号检测**：执行 `ls <结果目录>/info.json` 核验存在性（成功路径在 `aicerror_0_{errts}/` 子目录，退出码 8 路径在 `aicerror/` 子目录，见「一」两个写盘点）。判据是文件在不在，不检测 CANN 版本号——版本知识会过时，文件存在性是确定的。

**两类缺失归因**：

1. 版本不携带：CANN 9.3.0 及之前版本本就不产出 info.json（用户口径，待上游确认），属预期缺失，**不判故障、不要求升级 CANN**。
2. 旁路写失败：`write_json_report` 异常仅告警（json_report_builder.py:791-799），info.txt 已先行落盘，info.json 缺失不影响退出码。

**回退动作**：info.json 缺失时无缝回退 info.txt 既有判读（六段式判读规则原样保留），在诊断报告"环境与输入"段标注回退与归因。措辞示例：

> info.json 缺失，回退 info.txt 六段判读。归因：CANN 版本 ≤9.3.0 不携带该产物（用户口径）／旁路写失败（可查终端是否缺 `Analysis info (json) is saved in` 提示）。

**版本信息记录**：版本号在可得时（安装路径含版本号、用户提供）记录到报告"环境与输入"段，用于佐证归因；得不到时不臆测版本、只按文件存在性回退。
