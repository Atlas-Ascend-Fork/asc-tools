# msaicerr — AI Core 错误采集与解析

> 解析 device 采集的 AI Core error 报告，提取错误位置和关联信息；或运行内置算子样例检查软硬件环境。当通信算子触发 AI Core error / ECC / 硬件异常时使用。

## 工具定位

CANN 自带 AI Core 错误分析工具（`$ASCEND_HOME_PATH/tools/msaicerr/msaicerr.py`，Python 实现，依赖 `ms_interface/`）。

**前置依赖**：`ASCEND_OPP_PATH` 已设置（CANN 已 source set_env.sh）；当前目录可写。

**设备编号**：`-dev` 选择内置算子实际运行的 Device ID。结合当前运行时可见设备和工具帮助选择，不沿用另一台服务器的编号。示例中的 `DEVICE_ID` 表示已经核实的目标设备。

## 调用命令

```bash
case "${ASCEND_HOME_PATH:-}" in
  /*) ;;
  *) echo "require non-empty absolute ASCEND_HOME_PATH" >&2; exit 1 ;;
esac
SDK_REAL=$(realpath -m -- "$ASCEND_HOME_PATH") || exit 1
[ "$SDK_REAL" != "/" ] && [ -d "$SDK_REAL" ] || { echo "invalid ASCEND_HOME_PATH" >&2; exit 1; }
MSAICERR_PY="${SDK_REAL}/tools/msaicerr/msaicerr.py"
[ -f "$MSAICERR_PY" ] && [ -r "$MSAICERR_PY" ] || { echo "missing readable script: $MSAICERR_PY" >&2; exit 1; }

DEVICE_ID="${DEVICE_ID:-0}"  # 替换为已核实的目标逻辑 device id
case "$DEVICE_ID" in
  ''|*[!0-9]*) echo "invalid DEVICE_ID: $DEVICE_ID" >&2; exit 1 ;;
esac

# 1. 硬件/环境自检（仅当需要环境证据且允许占用设备时运行内置 golden 算子）
python3 "$MSAICERR_PY" -e -dev "$DEVICE_ID"
# 返回码：MS_AICERR_NONE_ERROR=内置样例在所测设备及当前窗口通过 / MS_AICERR_HARDWARE_ERR=内置样例失败（须结合设备日志与环境核实）

# 2. 分析 device 采集到的 AI Core error 报告目录
#    （通常由 msnpureport -a 或 device 自动落盘到 ~/ascend/log 或 /var/log/ascend/...）
REPORT_DIR="${REPORT_DIR:-./aicore_error_report}"
[ -d "$REPORT_DIR" ] || { echo "missing report directory: $REPORT_DIR" >&2; exit 1; }
python3 "$MSAICERR_PY" -p "$REPORT_DIR" -dev "$DEVICE_ID" -out ./aicerr_out
# ⚠ -out 应置于 -p 目录树之外（代码强制校验）

# 3. 解析某个算子 dump（配合 -dtype）
[ -f ./op_dump.bin ] || { echo "missing dump file: ./op_dump.bin" >&2; exit 1; }
python3 "$MSAICERR_PY" -d ./op_dump.bin -dtype fp32 -out ./parsed
```

**三种模式**：
| 模式 | 参数 | 说明 |
|------|------|------|
| 环境自检 | `-e [-dev <n>]` | 在指定 device 跑内置 golden 算子；通过仅证明所测样例在当前设备及窗口通过，失败须结合错误日志与环境核实 |
| 报告分析 | `-p <report_path> [-dev <n>] [-out <dir>]` | 解析 AI Core error 报告，输出错误码 + 关联算子 + 根因 |
| Dump 解析 | `-d <dump_file> [-dtype <dtype>] [-out <dir>]` | 把 device dump 的二进制转可读 |

**返回码**：`MS_AICERR_NONE_ERROR`（内置样例通过）/ `MS_AICERR_INVALID_PARAM_ERROR` / `MS_AICERR_INVALID_PATH_ERROR` / `MS_AICERR_INVALID_DUMP_DATA_ERROR` / `MS_AICERR_HARDWARE_ERR`（内置样例检测到环境或硬件异常）。

## 输出解读

- `-e`（自检）：返回 `MS_AICERR_NONE_ERROR`（内置样例在所测设备及当前窗口通过）或 `MS_AICERR_HARDWARE_ERR`（内置样例失败，须结合设备日志、错误码与环境版本核实）
- `-p`（分析）：输出目录含解析后的 AI Core 错误码、关联算子名、根因定位

## 诊断决策

| msaicerr 输出 | 指向 | 详见 |
|--------------|------|------|
| `-e` 返回 `MS_AICERR_HARDWARE_ERR` | 自检失败；结合具体错误、设备日志和环境版本确认原因，不仅凭返回码建议更换硬件 | |
| `-e` 返回 `MS_AICERR_NONE_ERROR` | 内置样例在所测设备上通过；这不覆盖原算子的全部路径或间歇故障，仍按当前证据检查 kernel 与同步 → [comm-operator-anomaly](../symptoms/comm-operator-anomaly.md) / [stream-event-cycle](../symptoms/stream-event-cycle.md) | |
| `-p` 解析报告关联到通信算子（CommTimeout/allreduce kernel） | 确认错误发生在该执行路径；继续核对错误类型、访问地址和同步证据，不能仅凭关联算子名判断实现 bug | |
| `-p` 报告目录为空或路径错误 | `tool_evidence` 标注 `skipped: no aicore error report collected`，提示先用 [msnpureport](msnpureport.md) `-a` 采集 | |

**何时调用**：
- [plog](plog.md) 含 `TS_ERROR`/`aicore error`/通信算子 kernel abort 时
- [msnpureport](msnpureport.md) bbox 显示 AI Core error 时
- 排查"偶发计算错误"是否硬件 ECC / 算子 kernel bug 时

## 降级策略

- `-p` 需要 device 采集到的 report 目录（路径需有内容），无 report 时先用 [msnpureport](msnpureport.md) `-a` 采集
- 工具依赖 `ASCEND_OPP_PATH`，未设置时报错：提示用户 `source set_env.sh`
- `-e` 会实际运行内置算子样例；只有需要环境证据且允许占用该设备时才执行，不把一次通过当作全部硬件健康的证明。

## 扩展查阅

- 当本页错误码表未覆盖当前输出的错误码、或选项语义不明时，查阅知识库中的错误码卡片并核对现场版本。
- 命令参数或语义本页未覆盖时，按知识访问说明检索固定版本知识卡并核对官方来源。

模式行为参考 [CANN 9.1 msaicerr 环境检查说明](https://www.hiascend.com/doc_center/source/zh/CANNCommunityEdition/910/maintenref/troubleshooting/troubleshooting_0534.html)。离线报告解析与实际设备自检应分开记录。
