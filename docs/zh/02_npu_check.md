# npu-check

## 概述

npu-check是面向昇腾NPU的运行时正确性检查工具，帮助客户在算子开发中进行正确性检查。通过npu-check启动应用，即可查看错误类型、发生位置和检查结果。

本手册面向Ascend C算子开发者，介绍环境准备、命令行用法、检查功能、报告阅读和典型使用示例。

通过Agent使用本工具时，可参考[tool-npu-check Skill](../../skills/tool-npu-check/SKILL.md)，了解适用范围、检查类型选择、受控运行、报告解读和算子复测。

### 检查功能

| 功能 | 主要用途 | 适合检查的问题 |
| --- | --- | --- |
| `memcheck` | 检查越界访问问题 | 算子是否存在越界访问，例如访问未分配的GM，或访问超出AI Core内Buffer大小的地址 |
| `synccheck` | 检查设备同步操作是否正确配对 | 重复设置事件、等待未设置的事件、设置事件后未等待，事件设置和等待不配对问题 |
| `initcheck` | 检查持久化寄存器在首次依赖使用前是否已被设置 | 搬运或计算指令依赖的持久化寄存器尚未设置，或者仅在使用后设置 |
| `racecheck` | 检查缺少必要同步的冲突内存访问 | 写后读、读后写、写后写竞争及原子写语义竞争 |

四种功能可以分别运行，也可以在同一次应用执行中同时启用。

### 使用场景

npu-check常见使用场景：

- 完成新算子后，检查正常输入范围、边界输入范围和尾块处理是否异常。
- 调整分块、搬运长度或stride后，检查访问是否异常。
- 修改流水间同步逻辑后，检查同步操作。
- 应用出现结果偶发错误或设备执行异常时，尝试定位问题。

正确的数值结果不代表代码一定正确。例如，程序可能已经读出了分配范围，但超出的数据没有参与最终计算，输出仍然正确。

## 环境准备

请参考[快速入门](00_quick_start.md)完成环境准备。使用前，请安装与目标NPU和驱动匹配的CANN软件包，并执行以下命令设置环境变量：

```bash
source ${INSTALL_DIR}/set_env.sh
```

> `${INSTALL_DIR}`请替换为CANN软件安装后的文件存储路径。以root用户安装为例，安装后文件默认存储路径为：`/usr/local/Ascend/cann`。

### 关于源码信息

若需要根据报告直接定位到源码文件和行号，编译算子时需要保留行号信息。

## 命令格式

```text
npu-check [--tool <checktype>]... [--log-file <filepath>] [--deadlock-timeout <seconds>] [--] <application> [args...]
```

### 选项

| 选项 | 输入 | 默认行为 | 说明 |
| --- | --- | --- | --- |
| `--tool <checktype>` | `memcheck`、`synccheck`、`initcheck`、`racecheck` | 未指定时启用`memcheck` | 可多次指定，用于同时启用多个工具；重复指定同一工具不会重复检查 |
| `--log-file <filepath>` | 文件路径 | 报告显示在终端 | 保存检查诊断信息 |
| `--deadlock-timeout <seconds>` | 正整数秒数 | 使用 Runtime 默认算子超时策略 | 设置算子执行超时时间 |
| `-h`、`--help` | 无 | 不显示帮助 | 显示命令帮助 |
| `--` | 无 | 可选添加 | 用于分隔工具参数和应用参数 |

## 使用示例

运行指定类型的检查，将`<checktype>`替换为`memcheck`、`synccheck`、`initcheck`或`racecheck`：

```text
npu-check --tool <checktype> ./my_app
```

不指定功能的情况下，默认使能内存访问检查（memcheck）：

```bash
npu-check ./my_app
```

使能全部检查：

```bash
npu-check --tool memcheck --tool synccheck --tool initcheck --tool racecheck ./my_app
```

### 指定报告输出位置

保存到指定文件：

```bash
mkdir -p reports
npu-check --tool memcheck --log-file reports/memcheck.log ./my_app
```

指定`--log-file`后，检查报告写入文件，不再在终端重复显示。应用程序的打屏行为不受影响。

### 设置算子执行超时

指定超时时间（单位：秒）：

```bash
npu-check --deadlock-timeout 5 ./my_app
```

上述命令将算子执行超时时间设置为5秒，可用于排查算子执行卡住的问题。算子执行超时时，Runtime 返回错误，应用按原有错误处理流程处理。

## 约束说明

- 当前设备检查范围为Ascend 950PR&950DT系列产品
- 当前支持通过`<<<>>>`和`aclnn`进行单算子单次调用
- `memcheck`支持检测指令的GM越界访问，以及L1、L0A、L0B、L0C中超出buffer边界的访问
- `synccheck`支持检测核内同步配对问题和重复设置同步问题，以及核间同步配对问题
- `initcheck`支持检测搬运和计算指令依赖的持久化寄存器初始化问题，以及GM、L1、L0A、L0B、L0C的内存初始化问题
- `racecheck`支持检测GM、L1、L0A、L0B、L0C的竞争访问问题
- `racecheck`支持ADD/MIN/MAX原子写检查。相同原子模式的写访问之间不视为竞争冲突；不同原子模式访问重叠地址且缺少必要同步时，报告原子写语义竞争。原子写与普通读写之间也参与竞争检查。
- 未启用原子模式的写访问按普通写检查。不支持或无法识别的原子配置会使竞争分析不完整。
- `racecheck`在单次launch内可报告多个竞争，发现首个竞争后继续检测其他内存区间；同一内存区间仅报告首次确认的竞争，后续竞争不重复报告。
- 目标程序必须能在未使用`npu-check`时独立运行
- 目标程序必须是可执行文件
- 不支持多进程和多线程运行
- 不支持在`simt vf`和`simd vf`场景下进行`racecheck`和`initcheck`检测
- 在使用`simt vf`和`simd vf`的场景下，`memcheck`只支持检测非`vf`代码产生的读写越界异常
- 算子使用非`inline`子函数时不支持检测
- `initcheck`内存初始化问题若属于竞争问题（例如写后读场景），则只能保证不误报；若存在竞争问题，工具会按预期报错。
- `synccheck`同步配对检查，以及`initcheck`、`racecheck`这种依赖同步关系的诊断，仅支持硬同步场景。若通过软件逻辑实现同步，则相关诊断结果不可靠。Ascend 950PR&950DT系列产品上支持检测的硬同步接口及范围如下：

  | 同步范围 | Ascend C基础API | 对应C API |
  | --- | --- | --- |
  | 核内流水间事件同步 | `SetFlag/WaitFlag`、`TQueSync::SetFlag/WaitFlag` | `asc_sync_notify/asc_sync_wait` |
  | 核内流水锁同步 | `Mutex::Lock/Unlock` | `asc_lock/asc_unlock` |
  | 核间模式0：全部AIC或全部AIV同步 | `CrossCoreSetFlag<0, pipe>/CrossCoreWaitFlag<0, pipe>` | `asc_sync_inter_arrive/asc_sync_inter_wait` |
  | 核间模式1：同一group内全部AIV同步 | `CrossCoreSetFlag<1, pipe>/CrossCoreWaitFlag<1, pipe>` | `asc_sync_subblock_arrive/asc_sync_subblock_wait` |
  | 核间模式2：同一group内AIC与全部AIV同步 | `CrossCoreSetFlag<2, pipe>/CrossCoreWaitFlag<2, pipe>` | `asc_sync_block_arrive/asc_sync_block_wait` |
  | 核间模式4：同一group内AIC与单个AIV同步 | `CrossCoreSetFlag<4, pipe>/CrossCoreWaitFlag<4, pipe>` | `asc_sync_intra_arrive/asc_sync_intra_wait` |
  | 全核硬同步 | 无workspace参数的`SyncAll<isAIVOnly>()`、`SyncAll<isAIVOnly, config>()` | 无 |

  若诊断信息显示高阶API内部存在同步不配对、初始化问题或竞争问题，则本次检出的诊断结果不可靠。
