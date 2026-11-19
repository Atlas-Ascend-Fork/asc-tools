# msobjdump 元数据说明

用于解释 dump 输出，不是所有版本或所有输入必含字段的清单。Binary与Function元数据分别打印，保留字段原名和数值。Function标题中的名称来自section名称后缀；相同名称可能对应多个section。缺少字段不代表对应功能关闭。

## 二进制级信息

| 字段 | 含义与解释边界 |
| --- | --- |
| `.ascend.meta META INFO` | 全局元数据信息的打印标题 |
| `VERSION` | 产物元数据中的版本值；不是 CANN 安装版本或 msobjdump 软件版本 |
| `DEBUG` | `debugBufSize` 是调试缓冲区大小，`debugOptions` 是调试选项；按对应版本定义解释，不能从它判断本次运行已产生调试数据 |
| `DYNAMIC_PARAM` | `dynamicParamMode` 表示动态参数模式；文档定义 0 关闭、1 开启 |
| `OPTIONAL_PARAM` | `optionalInputMode` / `optionalOutputMode` 表示可选输入/输出是否占位；文档定义 0 不占位、1 占位 |
| `RUNTIME_IMPLICIT_INFO` | 每条记录描述一种Runtime隐式资源，如Printf、硬同步、L2Cache Hint等；未知编号保留原值 |
| `SK_INFO` | `cap`为SK能力标志，`global`为Kernel函数入口地址，`sk0`至`sk3`为SK子函数入口地址，`reserved`为保留字段 |
| `SPECIALIZATION_RESOURCE_ID` | 算子二进制资源标识字符串 |
| `ICACHE_PRELOAD` | 预加载标志及保留字段；flag为0不预加载、1预加载 |
| `SPECIALIZATION_FLAG` | 二进制算子常量化状态：0为静态，1为动态 |

`debugOptions` 是位图：0表示关闭；1（DumpTensor/printf）、2（assert）、4（时间戳打点）、8（内存越界检测）可按位或组合。

## Function级信息

| 字段 | 含义与解释边界 |
| --- | --- |
| `.ascend.meta. [索引]: 名称` | 索引为打印序号，名称为section名称中`.ascend.meta.`之后的字符串；工具不查询函数符号，也不还原C++函数名 |
| `KERNEL_TYPE` | 产物声明的 core 类型；常见 AIC、AIV、MIX_AIC_MAIN、MIX_AIV_MAIN，分别指 Cube、Vector 及相应主核的混合类型；其他枚举按工具原值呈现 |
| `CROSS_CORE_SYNC` | 硬同步配置，`USE_SYNC` / `NO_USE_SYNC`；适用平台需结合当前 CANN 文档，不能扩展为所有芯片均支持 |
| `MIX_TASK_RATION` | Cube/Vector 任务配比，例如 `[1:2]`；保留工具原拼写，不解读为耗时或利用率比例 |
| `DETERMINISTIC_INFO` | 文档定义 0 非确定性、1 确定性计算；表示编译产物配置，不代替运行测试 |
| `FUNCTION_ENTRY` | 工具文档定义为 TilingKey；保留完整整数，不与符号地址、入口地址或真实执行过的分支混淆 |
| `BLOCK_NUM` | 当前未启用，`0xFFFFFFFF`为默认占位值 |
| `DEVICE_FUNC_TYPE` | 非Kernel函数类型；1融合架构MainScalar、2分离架构AIC MainScalar、3分离架构AIV MainScalar、4 SIMD函数、5 SIMT入口、6 SIMT子函数；与KERNEL_TYPE枚举不同，未知值保留编号 |
| `SIMT_LAUNCH_BOUNDS` | SIMT最大线程数；不表示实际启动线程数 |
| `DFX_INFO` | 异常调测使用的参数信息，由多个`DFX_ARG_INFO`记录组成 |
| `DFX_ARG_INFO` | 单个参数的异常调测信息，包括参数类别、指针层级及相应的数据大小或形状信息 |
| `L0_EXCEPTION_DFX_IS_TIK` | 是否为TIK算子：0为非TIK，1为TIK |
| `COMPILER_ALLOC_UB_SIZE` | 编译器分配的UB大小 |
| `SU_STACK_SIZE` | 核函数所需的Scalar Unit（SU）栈大小 |
| `SIMT_WARP_STACK_SIZE` | SIMT通用栈大小 |
| `SIMT_DVG_WARP_STACK_SIZE` | SIMT Divergence（分支）栈大小 |
| `AIV_TYPE_FLAG` | AIV执行类型：1为SU，2为SU + SIMD，3为SU + SIMT，4为SU + SIMD + SIMT |
| `PARAM_SUMMARY` | `paraNums`为参数数量，`paramTotalSize`为参数区总大小 |
| `PARAM_INFO` | 单个参数的布局：`ordinal`为从0开始的参数序号，`offset`为参数区内偏移，`size`为参数槽位大小，`align`为对齐要求；`paramType`中1为标量、2为指针 |
| `SCHED_MODE` | 调度模式：0为普通模式，1为Batch模式 |
| `ENABLE_EARLY_START` | 当前源码识别并打印该字段整数；仅凭值不能断言early start特性已在运行中生效或取得加速 |

例如输出 `KERNEL_TYPE: MIX_AIC_MAIN` 与 `MIX_TASK_RATION: [1:2]`，可以说明产物采用相应的混合核配置，不能由此推算算子耗时。`mix_aic` / `mix_aiv` 两个符号可以是同一混合算子的两部分，不能直接当作两个独立算子。

## verbose 与输入格式

verbose 额外提供 ELF header、section、program header 和 symbol 等信息，是结构信息而不是指令反汇编；device 指令反汇编由 `--sass` 完成。分析融合编译产物时，详细信息可能来自提取后的 device ELF，而不是外层 host ELF；解释架构和入口时标明对象。

| 内容特征 | dump/list/extract 处理方式 | `--sass` 处理方式 |
| --- | --- | --- |
| `.ascend.meta.*` | 直接读取Function元数据；单算子 ELF 没有内嵌文件时 list/extract 提示无内容 | 按元数据定位后再反汇编 |
| 独立 device ELF（machine 为 `hiipu` 等） | 直接解析 | 直接反汇编 |
| `.ascend.kernel.*` | 按内部描述读取 kernel 二进制并列出或提取 | 提取各 kernel 后逐个反汇编 |
| `_o_start` / `_json_start` 等符号 | 识别包含二进制及 JSON 的打包产物，支持相应文件恢复 | 恢复 `.o` 负载后反汇编 |
| `.aicore_binary` | 使用 llvm-objcopy 提取 device 段，再读取元数据；列表名形如 `demo.aicore.o` | llvm-objcopy 提取后反汇编，来源标注 `demo[.aicore_binary]` |
| `.a` 归档 | FileAction 预处理按首成员 `ar x` 提取，可能落盘当前目录；不代表任意成员均支持 | 遍历全部成员（`ar p` 写入临时目录，不污染当前目录）；thin archive 明确不支持 |

这些是内容识别线索，不是扩展名白名单。无法识别时保留实际输出，不能统一归因为文件损坏。`--sass` 对嵌套容器最多向下遍历 4 层。

## sass 反汇编输出

`--sass` 调用所选 CANN 的 llvm-objdump 反汇编 device ELF，输出形如：

```text
===== [SASS] /path/demo[.aicore_binary] =====

/tmp/.../aicore_xxx.o:    file format elf64-hiipu

Disassembly of section .text:

0000000000000000 <__init_kfc_workspace_addr.cube>:
       0:                07027fff    MOV         X1, #32767
       4:                02050880    MOV        X2, COREID
```

每行为“地址、十六进制编码、指令助记符与操作数”。`<函数名>` 块按符号划分，不等于 kernel 边界。`file format` 行指明反汇编对象架构；融合产物的指令来自提取后的 device ELF，不是外层 host ELF。

`<not available>` 表示当前 llvm-objdump 没有该架构的反汇编后端，不是文件损坏；msobjdump 会将其判定为失败并报错。指令语义、调度与性能不能凭静态反汇编直接推断，需结合架构手册与实测。

## 依据

以下路径位于 asc-tools 仓库；本页已保留使用所需含义，独立分发 Skill 时不依赖仓库在固定本机路径存在。

- `docs/zh/03_msobjdump.md`：命令格式和字段定义。
- `examples/04_msobjdump/README.md`：融合编译产物的输出示例，含 `--sass` 用法。
- `utils/msobjdump/msobjdump/msobjdump_main.py`：`B_TYPE_MAP`、`F_TYPE_MAP`、`K_TYPE_MAP`、`_show_ascend_meta_tlv`、`_show_ascend_meta_op_tlv`、`_detect_obj_type`、`_classify_sass_file`、`_collect_sass_images`、`_disassemble_sass_image`。
- `scripts/package/asc-tools/`：`npu-objdump` 主命令与 `msobjdump` 兼容软链的安装与卸载脚本。
