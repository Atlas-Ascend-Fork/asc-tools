# msnpureport — device 日志导出

> 导出 device 侧全部日志 / bbox / 栈 / 寄存器，并支持运行时动态调整 device 日志级别。用于排除或确认 device 侧（AICPU/HCCS 链路）硬件问题。

## 工具定位

driver 自带 device 维测工具（`/usr/local/Ascend/driver/tools/msnpureport`，root 所属）。当 plog 落盘信息不足以判断是否 device 侧硬件故障时使用。

**设备编号**：按当前 `msnpureport` 帮助及驱动文档确认 `-d` 的编号含义和可见范围，不假设它与 ACL 逻辑编号、hccn_tool 的 Phy-ID 或旧服务器编号相同。示例中的 `REPORT_DEVICE_ID` 需取自本次核对。

## 调用命令

先利用已提供的日志。下面包含改变日志级别和全量导出的按需命令，不要因为使用技能就执行全部操作；需要改变级别时记录原值，并在采集结束后恢复。

```bash
REPORT_DEVICE_ID="${REPORT_DEVICE_ID:-0}"  # 替换为当前工具帮助与设备映射核实后的目标 ID
case "$REPORT_DEVICE_ID" in
  ''|*[!0-9]*) echo "invalid REPORT_DEVICE_ID: $REPORT_DEVICE_ID" >&2; exit 1 ;;
esac

# 1. 复现前调高 device 日志级别（配合 host 侧 ASCEND_GLOBAL_LOG_LEVEL=0）
/usr/local/Ascend/driver/tools/msnpureport -g debug          # 修改日志级别前先确认作用范围
/usr/local/Ascend/driver/tools/msnpureport -g info           # 调回 info
/usr/local/Ascend/driver/tools/msnpureport -m FMK:debug -d "$REPORT_DEVICE_ID" # 仅指定 device 的 FMK 模块 debug

# 2. 异常发生后取证
/usr/local/Ascend/driver/tools/msnpureport -a                # 导出全量日志 + bbox event（推荐取证）
/usr/local/Ascend/driver/tools/msnpureport -t 2              # 仅 bbox + 寄存器
/usr/local/Ascend/driver/tools/msnpureport -t 1              # 仅 slog/message/system_info
/usr/local/Ascend/driver/tools/msnpureport -f                # 更彻底的全量 + 历史维护信息（送服用）

# 3. 查询当前级别
/usr/local/Ascend/driver/tools/msnpureport -r
```

**关键参数**：
| 参数 | 说明 |
|------|------|
| `-g <level>` | 设置 device 全局日志级别：error/info/warning/debug/null |
| `-m <MODULE>:<level>` | 设置指定模块日志级别（如 FMK:debug） |
| `-d <id>` | device ID（逻辑 id 0/1），仅与 -g/-m/-e/-r 配合 |
| `-t <type>` | 导出类型：0=所有 / 1=slog/message / 2=bbox+寄存器 / 3=stackcore / 4=vmcore |
| `-a` | 导出所有日志文件 + bbox 中的 device event |
| `-r` | 查询当前 device 日志级别 |
| `--docker` | docker 内使用时必须显式加该 flag |

## 输出解读

- `-a` / `-t 2` 导出 **bbox**（device 异常时的寄存器/栈快照），含 HCCS 链路错误码、AICPU 异常状态、ECC 错误地址等
- `-r` 返回当前 device 全局日志级别（确认复现前是否已调高）

## 诊断决策

| msnpureport 输出特征 | 指向 | 详见 |
|---------------------|------|------|
| bbox 含 HCCS 链路错误 / ECC 错误 | `anomaly_module = 硬件`，根因偏向 device 侧硬件故障 → 触发 [msaicerr](msaicerr.md) 解析 AI Core error | |
| bbox 无异常 / 导出正常 | 这份报告中未发现相应异常；先核对时间范围和采集完整性，再结合其他证据检查 rank 参与对等性 / 同步 / 配置 → [communication-timeout](../symptoms/communication-timeout.md) | |

**何时调用**：
- [plog](plog.md) 的 device-*/.log 含 `AICPU timeout`/`TS_ERROR`/`ECC`/`SVID`/`HWERR` 时
- 排查需排除 device 侧硬件/链路故障时
- `anomaly_module` 倾向硬件/链路时

## 降级策略

- 工具属 root，普通用户执行读写级别设置可能受限：`tool_evidence` 标注 `skipped: requires root`，降级为基于 [plog](plog.md) device 侧日志的诊断
- 导出大文件需写权限目录，若无写权限则跳过完整导出，仅用 `-r` 查询级别 + [plog](plog.md) device 日志兜底

## 扩展查阅

- 当本页未覆盖当前导出的日志/bbox 报告内容、或选项语义不明时，查阅知识库中 msnpureport 相关卡片、现场帮助和授权文档。
- 命令参数或语义本页未覆盖时，按知识访问说明检索固定版本知识卡并核对官方来源。
