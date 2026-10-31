# optype_collector

## 概述

`optype_collector` 用于采集 CANN OPP 包中指定 SoC 的 OpType 信息，并检测自定义算子包和内置算子包、自定义算子包之间的 OpType 重名问题，帮助开发者在自定义算子安装或交付前提前发现命名冲突。

工具支持以下信息来源：

- `<CANN安装目录>/cann/opp/built-in` 下的 CANN 内置算子包。
- `<CANN安装目录>/cann/opp/vendors` 下的自定义算子包。
- `ASCEND_CUSTOM_OPP_PATH` 指定的一个或多个自定义算子包路径。

## 使用前准备

工具随 asc-tools 出包安装。安装完成并配置 CANN 环境变量后，用户无需进入工具目录，可直接通过 `optype_collector` 命令调用。

## 环境变量

使用前需配置 CANN 环境变量：

```bash
source <CANN安装目录>/cann/set_env.sh
```

工具依赖的环境变量如下：

| 环境变量 | 说明 |
| ---- | ---- |
| `ASCEND_CUSTOM_OPP_PATH` | 自定义算子包路径。可选，未设置时仍会扫描 `<CANN安装目录>/cann/opp/vendors`。多个路径使用系统路径分隔符分隔。 |

## 命令格式

`{soc_version}` 为目标 SoC 名称，可填写 CANN 支持的产品 SoC 名称。

已有脚本中的全小写 SoC 名称仍可继续使用。

- 输出内置算子 OpType 清单：

  ```bash
  optype_collector {soc_version}
  optype_collector {soc_version} --builtin
  ```

- 输出自定义算子 OpType 清单：

  ```bash
  optype_collector {soc_version} --custom
  ```

- 输出内置算子与自定义算子的全部 OpType 清单：

  ```bash
  optype_collector {soc_version} --all
  ```

- 检测 OpType 重名冲突：

  ```bash
  optype_collector --detect-conflicts {soc_version}
  ```

## 返回值

| 返回值 | 说明 |
| ---- | ---- |
| `0` | 执行成功，且冲突检测模式下未发现重名冲突。 |
| `1` | 冲突检测模式下发现 OpType 重名冲突。 |
| `2` | 参数错误、环境变量缺失、目标 SoC 包不存在等阻塞类错误。 |

## 输出说明

工具每次执行都会输出以下信息：

- 扫描信息：展示输入 SoC、`ASCEND_HOME_PATH`、`ASCEND_CUSTOM_OPP_PATH`。
- 信息来源：展示每个命中来源的来源类型、状态、配置文件数量、OpType 数量、输入 SoC 和路径。
- OpType 清单或检测结果：根据命令模式输出 OpType 列表或冲突分组。
- 警告和错误：未设置 `ASCEND_CUSTOM_OPP_PATH`、未找到自定义算子包等信息会在扫描结束后统一输出。

当输入 SoC 不受支持时，工具返回阻塞类错误，并提示：

```text
Error: SoC version is not supported: {soc_version}
```

冲突检测模式会覆盖以下两类冲突：

1. 自定义算子包与 CANN 内置算子包之间的 OpType 重名。
2. 不同自定义算子包之间的 OpType 重名。

## 示例

```bash
# 查看 {soc_version} 内置算子 OpType
optype_collector {soc_version} --builtin

# 查看 {soc_version} 自定义算子 OpType
optype_collector {soc_version} --custom

# 检测 {soc_version} 下自定义算子与内置算子、自定义算子之间的重名冲突
optype_collector --detect-conflicts {soc_version}
```
