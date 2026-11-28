# npu-compute CLI 使用说明

## 命令格式

```text
npu-compute [options] [--] [program] [program-arguments]
npu-compute --import <report> [--export <path>]
```

采集时必须指定目标程序，可用绝对路径、相对路径或 `PATH` 中的命令。工具选项必须放在目标程序之前。`--` 后第一项是目标程序，目标程序名以 `-` 开头时必须使用该分隔符。程序之后的参数原样传入，生成命令时保留每个参数的边界。脚本无执行权限时，可用对应解释器启动。

## 选项

| 选项 | 含义 |
|---|---|
| `-h`、`--help` | 显示帮助。与其他有效选项组合时只显示帮助。 |
| `--list-sets` | 列出当前版本的 Set 及其有序 Section 成员。 |
| `--list-sections` | 列出当前版本的 Section。 |
| `--set <name>` | 选择预定义的指标组集合。 |
| `--section <name>` | 选择单个指标组。 |
| `--replay-mode <mode>` | 默认为 `kernel`。使用 `application` 时，显式选择 `Communication` 作为唯一 Section，省略 Set 和 `--cache-control`。 |
| `--cache-control <mode>` | 仅用于 Kernel 模式。默认 `none`，`all` 在每轮重放前尝试清理 L2。 |
| `--no-pack` | 直接保留采集文件目录，不生成 `.npu-rep`。 |
| `-i`、`--import <report>` | 解包已有 `.npu-rep`，可配合 `--export` 指定已有父目录。 |
| `-o`、`--export <path>` | 指定报告文件路径或结果父目录，见下文。 |

查询选项单独使用，各自只指定一次且不带值。解包模式使用 `--import` 和可选的 `--export`。

## 选择指标

- 少量指定指标用一个或多个 `--section`，常用指标用 `--set basic`，完整 Kernel 指标集用 `--set full`。
- 未指定指标时，省略 `--set` 和 `--section`，工具默认使用 `basic`。显式选择指标时只采集已选 Section。
- 名称区分大小写。按用户表达顺序生成选项。两种选项可重复、交错使用，工具按命令行出现顺序展开 Set，并按 Section 首次出现的位置去重。
- 用当前安装版本的查询结果核对成员。离线时可查 [Section 目录](../assets/section-catalog.json)。`Communication` 仅适用于 Application Replay，在两个 Set 之外。

```bash
npu-compute --list-sets
npu-compute ./add_custom
npu-compute --set basic ./add_custom
npu-compute --set full ./add_custom
npu-compute --set basic --section ResourceConflictRatio ./add_custom
npu-compute --section Memory --set basic ./add_custom
npu-compute --set basic -- ./add_custom --help --section app-value
npu-compute --section Memory --section L2Cache bash run.sh
```

## 报告路径

| 模式 | `--export` 可用值 | 终端输出 |
|---|---|---|
| 打包采集 | 尚不存在的 `.npu-rep` 文件路径，或用于保存自动命名报告的已有目录。 | `npu-compute: report=` |
| `--no-pack` 采集 | 已有父目录，工具在其中创建唯一的结果子目录。 | `npu-compute: directory=` |
| 报告解包 | 已有父目录，工具在其中创建唯一的解包子目录。 | `npu-compute: unpacked=` |

省略 `--export` 时使用当前目录。父目录须已存在，已有报告不会被覆盖。

```bash
npu-compute --cache-control all --section L2Cache --export ./cache.npu-rep ./add_custom
npu-compute --replay-mode application --section Communication --no-pack --export ./results ./application
npu-compute --import ./cache.npu-rep --export ./unpacked-results
```
