# 检查能力

`npu-check` 提供 `memcheck`、`synccheck`、`initcheck` 和 `racecheck` 四种检查能力。

运行配置和结论边界见[能力范围与限制](capability-boundaries.md)。

| 命令行选项 | 启用的检查能力 |
| --- | --- |
| `--tool memcheck` | 检查受支持指令的 GM 分配范围及 L1、L0A、L0B、L0C 容量边界 |
| `--tool synccheck` | 检查核内配对及受支持的跨核同步模式 |
| `--tool initcheck` | 检查持久化寄存器依赖和受支持内存空间的初始化状态 |
| `--tool racecheck` | 检查同一次下发内的内存访问竞争 |
| 多个 `--tool` | 在同一次命令中启用所选检查，重复同一工具不重复检查 |

## memcheck 能力与边界

`memcheck` 检查本次运行中已支持指令的 GM 访问和 L1、L0A、L0B、L0C 容量越界，包括部分 scalar 访问。
这不代表所有指令或所有存储空间都受支持。报告可提供：

- `read` 或 `write` 访问方向及字节数；
- 源码文件和行号，或 NPU 核函数（kernel）名称与指令地址（PC）；
- 执行核类型（AIC/AIV）、核编号（core）、任务块（block）、搬运流水（pipe）和下发批次（launch）等执行
  信息；
- 异常地址或片上偏移，以及 GM 特定错误中的分配基址、大小或生命周期信息；片上越界可提供空间容量和有效偏移范围；
- NPU 侧调用栈（报告原文为 `Device Frames`），用于定位到用户算子中的调用点。

GM 越界判定以受支持分配接口及 `aclrtFree` 的成功返回结果记录的有效 NPU 内存分配范围为基准。
`aclrtMalloc`、`aclrtMallocAlign32` 请求大小向上按 32 字节对齐后作为检查边界；`aclrtMallocWithCfg` 按请求字节数
记录，而不是使用张量（Tensor）的逻辑输入形状。判定依据、失败调用处理和覆盖限制见
[GM 越界判定基准](capability-boundaries.md#gm-越界判定基准)。

L1/L0 按对应空间容量检查，详见[片上容量判定基准](capability-boundaries.md#片上容量判定基准)。
片上容量不是用户逻辑 buffer 的大小；不能将 GM 分配范围规则套用到 L1/L0 报告，也不能将容量内访问解释为
所有 buffer 边界均正确。

检测数据在应用调用 `aclrtSynchronizeStream` 或 `aclrtSynchronizeStreamWithTimeout` 时导出并处理。应用不调用任一
接口时，不能依据没有诊断判断本次检查通过。

这些字段共同描述本次已执行访问的空间、地址范围和执行信息，不代表工具检查了张量的逻辑输入形状、数值精度或
性能。能力与结论边界见[能力范围与限制](capability-boundaries.md)。

## synccheck 能力与边界

当前核内配对检查包括：

```text
SET_FLAG -> WAIT_FLAG
GET_BUF  -> RLS_BUF
```

这两组关系必须成对出现。`synccheck` 根据本次实际执行记录中的指令参数和实际执行次数
判断是否配对；未实际执行的控制流不在本次结论范围内。

报告汇总中可能出现以下英文原始字段：

- `duplicate_opens`：同一组配对参数（报告中记为 `key`）的 `SET_FLAG` 或 `GET_BUF` 尚未执行对应的
  `WAIT_FLAG` 或 `RLS_BUF`，就再次执行；
- `unmatched_closes`：执行了 `WAIT_FLAG` 或 `RLS_BUF`，但没有相同配对参数的 `SET_FLAG` 或 `GET_BUF`；
- `unconsumed_opens`：应用执行流同步时，`SET_FLAG` 或 `GET_BUF` 仍未执行对应的 `WAIT_FLAG` 或 `RLS_BUF`。

判断是否匹配时应读取报告中的配对参数，而不是只比较操作名称。报告中的 `launch` 字段表示本次诊断对应的核函数
下发编号。

此外，当前实现支持部分跨核同步模式，包括 `FFTS_CROSS_CORE_SYNC/WAIT_FLAG_DEV`。此类报告需结合模式、flag、
参与核和拓扑解读，不能只用核内两两配对规则解释，也不代表所有跨核模式均受支持。

## initcheck 能力与边界

检查受支持搬运、Fixpipe 等指令在使用持久化寄存器前是否完成设置，并检查 GM、L1、L0A、L0B、L0C 的未初始化读取。
报告可区分寄存器使用前未设置、完全或部分未初始化内存读取，并给出相关位置及访问上下文。
内存报告中的读取总长度与未初始化字节数不同；初始化状态也不能替代 memcheck 的地址有效性判断。
指令、访问范围或同步信息不完整时，不把没有未初始化诊断解释为完整通过。

## racecheck 能力与边界

检查同一次核函数下发内缺少必要同步的 RAW、WAR、WAW 及原子写语义竞争，不检查不同下发之间的竞争。
支持 ADD/MIN/MAX 原子写；相同模式原子写之间不视为竞争，不同模式原子写或原子写与普通读写之间仍可能冲突。
不支持或无法识别的原子配置会限制分析完整性。同一内存区间仅报告首次确认的竞争，不能把报告条数当作动态竞争总数。
竞争检查、初始化检查和地址范围检查分别回答不同问题，用户指定一种检查时，不自动替换或追加其他类型。

## 命令行行为

未提供任何 `--tool` 时，当前命令行默认启用 `memcheck`。显式写出检查类型可明确记录本次启用的能力。用户指定
检查类型时遵循其选择；若该能力不能覆盖用户询问的检查对象，应说明覆盖边界，不自动替换或追加其他检查类型。

应用被外部超时或信号终止且 `npu-check` 未收到完整报告时，无法获得有效检查结论。此时按
[运行 npu-check](execution.md)保留原始输出和终止信息，并将本次检查判定为未完成。
