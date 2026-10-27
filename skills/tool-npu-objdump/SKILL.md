---
name: tool-npu-objdump
description: 使用 msobjdump 查看 Ascend C 算子编译产物的 kernel 元数据、列出或提取内嵌 device ELF、反汇编 device 侧 AI Core 指令，并定位工具调用问题。当用户询问 msobjdump 用法、分析 Ascend 算子 ELF、查询核类型或 TilingKey、提取 device 文件、反汇编算子 AI Core 指令时使用。不用于 host 侧（x86/AArch64 等）普通 ELF 反汇编或 NPU 运行性能采集。
---

# Ascend 算子产物查看、提取与指令反汇编

通过已安装的 msobjdump CLI 分析已有编译产物。按用户目标选择操作，不执行输入程序。安装包主命令为 `npu-objdump`，`msobjdump` 是指向它的兼容软链接，参数与行为一致。

## 工作流程

### 1. 确定请求

- **用法咨询**：解释环境和对应命令，不要求用户先提供 ELF，也不为了演示自动编译工程。
- **文件分析**：复用用户提供的文件与环境。用户只说“看看这个 Ascend 算子文件”时默认 dump；输入路径缺失且上下文无法确定时再询问。
- **问题定位**：围绕实际命令、输出和相关依赖检查，按需读取 [故障排查](references/troubleshooting.md)。

一个没有扩展名的可执行文件也可能含 device ELF；不能仅凭 `.o`、`.so` 或 `.a` 后缀判断工具支持。device 侧 AI Core 指令反汇编属于本流程（`--sass`）；host 侧（x86/AArch64 等）普通反汇编与性能采集使用 GNU/LLVM objdump 等相应工具，不因出现 ELF 一词而触发本流程。

### 2. 准备环境

已有可用环境直接复用。需要加载 CANN 时，使用用户指定或当前上下文已确定的安装目录；多个版本不能仅根据目录名猜测“最新”。在同一 Bash 进程中加载并调用：

```bash
source "${cann_root}/set_env.sh"
msobjdump -h
```

示例中的 `cann_root` 是已确认的 CANN 安装目录变量，使用前设置；不将个人机器路径写入命令模板。新工具调用若创建新 shell，需要重新加载环境。

| 依赖 | 何时需要 |
| --- | --- |
| Python 3、msobjdump 命令及 Python 包 | 运行工具；先用 `msobjdump -h` 检查 |
| `readelf` | ELF section、symbol 和 header 读取 |
| `llvm-objcopy` | 处理 `.aicore_binary` 融合编译产物（dump/list/extract 与 `--sass` 均可能触发） |
| `ar` | 处理 `.a` 静态库输入 |
| `llvm-objdump` | 仅 `--sass` 反汇编时需要；必须来自带 Ascend device 架构反汇编后端的 CANN 环境 |

这些依赖均为按路径的条件项，除工具本体和 `readelf` 外缺失不影响其他操作；不把 NPU 设备、驱动检查、ASCConfig.cmake 或编译工程作为分析已有文件的前置条件。缺依赖时说明缺项和配置方法，不自动安装软件或修改 shell 启动文件。

### 2.1 接口自检（无状态）

安装的 msobjdump 可能比本 Skill 记载的接口更旧或更新。首次实际分析、切换 CANN 包或工具路径、遇到未知参数/字段或文档与输出冲突时，运行一次：

```bash
python3 skills/tool-npu-objdump/scripts/check_interface.py --json
```

脚本无状态、运行期不写任何文件：拿当前环境的 `msobjdump -h` 与 [interface.json](references/interface.json) 快照比对，并探测 npu-objdump/msobjdump 命令与 Python 模块来源。不要把 `ok` 当成输入格式已经成功解析：格式能力仍需真实样本验证。

按状态处理：

- `ok`：记载的必备参数齐备。若带 `optional_missing_options`（如 `--sass`），说明当前安装早于该能力，按安装版实际能力回答并注明升级后才可用。
- `newer-install`：安装出现了 Skill 未记载的选项（快照落后于安装）。先核对 asc-tools 变更或官方文档再使用，不凭选项名猜行为。
- `missing-options`：安装缺少记载的必备参数，过旧或不兼容。报告当前工具限制，保留旧指导作参考，不声称支持。
- `unavailable` / `failed`：命令不存在或 `-h` 失败，先 source 匹配的 CANN 修正环境。

快照与文档的持久更新只能走 PR：用 `scripts/gen_interface.py` 从源码重新生成 `interface.json`，语义变化同步修订 SKILL.md、references 和 eval；改动 msobjdump 接口的 MR 应附带快照更新（`gen_interface.py --check` 可在合入前自查）。详细规则见 [自检与更新规则](references/update-policy.md)。

纯用法咨询且用户明确不执行命令时，不运行探测；可说明实际使用时的自检入口和限制。

### 3. 选择命令

| 用户目标 | 调用方式 |
| --- | --- |
| kernel 名称、核类型、TilingKey 等元数据 | `msobjdump --dump-elf "$input_file"` |
| ELF header、section、symbol 等完整信息 | `msobjdump --dump-elf "$input_file" --verbose` |
| 列出内嵌 device ELF | `msobjdump --list-elf "$input_file"` |
| 提取文件 | `msobjdump --extract-elf "$input_file" --out-dir "$output_dir"` |
| 反汇编 device 侧 AI Core 指令 | `msobjdump --sass "$input_file"` |
| 反汇编并显示文件名和行号 | `msobjdump --sass "$input_file" --lineinfo` |
| 显示源码和反汇编 | `msobjdump --sass "$input_file" --source` |
| 同时显示行号和源码 | `msobjdump --sass "$input_file" --lineinfo --source` |

`--lineinfo`显示源文件名和行号；`--source`/`-s`在反汇编结果中显示对应源码。使用这两个选项时需要指定`--sass`。

编译算子时请使用 `-g` 生成调试信息，配置方法请参见[常用的编译选项](https://gitcode.com/cann/asc-devkit/blob/master/docs/zh/guide/programming_guide/compilation_and_execution/operator_compilation/ai_core_operator_compilation.md#ZH-CN_TOPIC_0000002462746461)和[自定义编译选项](https://gitcode.com/cann/asc-devkit/blob/master/docs/zh/guide/programming_guide/advanced_programming/aclnn_operator_development/compilation_and_deployment/basic_process.md#custom-compile-options)。

显示源码时需保留调试信息中记录的源文件及其路径。即使device ELF不含调试信息，`--sass`仍可输出反汇编指令，但`--lineinfo`无法显示源码位置，`--source`无法显示源码。使用`--source`时，若源文件无法访问，工具会输出告警并继续反汇编，但不显示对应源码。

`input_file` 使用已确认的绝对路径，`output_dir` 在调用前创建并解析为绝对路径。一个调用只带 dump/list/extract/sass 中一个主操作；多个目标分别调用——新版 argparse 对组合使用直接报错退出 2（如 `--sass cannot be combined with an existing ELF action`），旧版则可能静默按第一个参数执行，都不能拼在一起当批量操作。`--verbose` / `-V` 是 dump 的详细模式，不是版本查询。不能写成 `msobjdump 文件名`。普通查询不自动执行提取或反汇编，也不默认执行全部操作。

`--sass` 输出打印到 stdout，保存结果用 Shell 重定向（如 `> "$output_file"`），不能与 `--out-dir` 组合。它支持独立 device ELF、融合编译 host ELF（自动提取 `.aicore_binary` 后反汇编）、`.a` 静态库（自动遍历成员到临时目录，不污染当前目录）以及 `.ascend.kernel.*`、`_o/_json` 封装；thin archive 不支持。

**路径含空格或 Shell 通配字符时**：当前已安装 CANN 的 Shell 入口以未引用的 `$@` 转发参数，外层引号无法阻止入口内部拆词或展开，表现为 `unrecognized arguments` 或“文件不存在”；asc-tools 新版入口已修复为 `"$@"`，随后续安装包发布。遇到拆词报错时先确认入口与安装版本，再用同包模块 CLI 规避：

```bash
python3 -c 'import msobjdump; print(msobjdump.__file__)'
python3 -m msobjdump --dump-elf "$input_file"
```

包来源一致后，可用 `python3 -m msobjdump` 替代命令前缀，保持后续参数不变；这仍是相同 Python 包的 CLI。不要静默切换到另一套包或修改用户文件名。包来源不一致时先修正当前环境。

### 4. 执行与呈现

在独立可写工作目录中执行，保留输入绝对路径。list/dump 也会生成临时文件，静态库成员可能提取到当前工作目录，不能把“查询”理解成零磁盘写入。

提取采用用户指定目录；没有指定则创建独立结果目录并告知位置。已有文件可能被工具覆盖：目标非空且无法确定是否冲突时，优先使用新的子目录并告知；用户要求精确落盘位置时先澄清覆盖意图。只清理本次创建的临时内容，不删除用户输入或已交付的结果文件。

结合退出码、stdout、stderr 和实际产物判断操作结果。新版工具已传播多数错误（运行错误退出 1，参数互斥或输入不存在退出 2），但部分提示仍以 warning 形式伴随零退出码出现，extract 也可能成功而不打印提示。提取后列出实际文件并检查非空，不能只凭“命令执行完”声称成功。

只提供提取命令而不执行时，也应说明调用后的文件检查步骤；不要把 `&&` 或退出码为零当成提取成功的充分条件。

- dump：按 kernel 给出相关字段，需要解释时读取 [元数据说明](references/metadata.md)。未提供的字段不补默认值，未知值原样保留。
- verbose：摘录与问题相关的结构；输出过长时保存完整日志并给出路径。
- list：展示实际列出的文件名；单算子 ELF 的 `nothing to list` 表示没有内嵌文件，不是文件损坏。
- extract：给出实际生成文件的路径；`nothing to extra` 表示该输入没有可提取内容，不声称成功提取。
- sass：结果打印到 stdout，多个 device 镜像以 `===== [SASS] 来源 =====` 分隔，来源标注提取路径（如 `demo[.aicore_binary]`、`lib.a(member.o)`）。退出码 0 要求找到镜像且无错误诊断。源码文件无法访问会产生 warning，但该告警本身不会使有效指令反汇编失败，对应源码不会显示。无 device ELF、thin archive、`llvm-objdump` 缺失或其输出全为 `<not available>`（当前环境的 llvm-objdump 不带 Ascend 反汇编后端）都会报错并返回非零；此时按 [故障排查](references/troubleshooting.md) 定位环境，不要声称已反汇编。

回答围绕用户问题组织，不强制生成固定报告。保留可复现命令和必要证据。元数据说明编译产物配置，不能证明真实运行核数、精度或性能收益；`BLOCK_NUM: 0xFFFFFFFF` 是当前实现的占位输出。

## 参考资料

- [元数据说明](references/metadata.md)：解释字段、区分全局与 kernel 信息时读取。
- [故障排查](references/troubleshooting.md)：遇到依赖、格式、路径或版本问题时读取。
- [自检与更新规则](references/update-policy.md)：来源或 CLI 发生变化时读取。

本 Skill 的知识依据为 asc-tools 的 `docs/zh/03_msobjdump.md`、`examples/04_msobjdump/README.md` 和 `utils/msobjdump/msobjdump/` 实现。具体格式能力以当前工具与输入实际表现为准。
