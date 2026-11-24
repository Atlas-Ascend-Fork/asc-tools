# plog — 运行时日志系统

> 多卡通信异常诊断的**首选日志源**。所有进程（host 侧 HCCL/RUNTIME/TDT/OP 与 device 侧 CCECPU/AICPU）日志均经它落盘。

## 工具定位

plog 是 Ascend 运行时日志系统（slog/plog 组件），非独立 CLI，由 `slogd` daemon 写入。通信超时 / rank 等待 / collective 不一致类异常的第一证据来源。

**日志落盘位置**（标准可移植路径）：
- 根目录：`~/ascend/log/`
- host 侧进程 plog：`~/ascend/log/run/plog/plog-<pid>_<timestamp>.log`
- host 侧 DEBUG/异常 plog：`~/ascend/log/debug/plog/`（ERROR/FATAL 落这里）
- device 侧（CCECPU/AICPU）：`~/ascend/log/run/device-<N>/device-<hostpid>_<ts>.log`

## 调用命令

```bash
# 1. 重跑前调高日志级别（0=DEBUG 最详细；1=INFO 可见 comm config 与 HCCL_ENV）
export ASCEND_GLOBAL_LOG_LEVEL=0
export ASCEND_SLOG_PRINT_TO_STDOUT=0   # 0=落盘到 plog 目录（默认）；设为 1 则改打终端标准输出
# 启动业务复现异常

# 2. 定位最新 plog 文件
ls -t ~/ascend/log/run/plog/ | head      # host 侧进程
ls -t ~/ascend/log/debug/plog/ | head    # ERROR/FATAL
ls -t ~/ascend/log/run/device-*/ | head  # device 侧

# 3. 按通信异常特征关键词过滤（先将 PID 设为步骤 2 核实后的纯数字进程号并校验格式）
LOG_ROOT="$HOME/ascend/log"
PID="12345"  # 替换为步骤 2 确认的目标进程纯数字 PID
case "$PID" in
  ''|*[!0-9]*) echo "invalid PID: $PID" >&2; exit 1 ;;
esac
shopt -s nullglob
plog_files=("$LOG_ROOT/run/plog/plog-${PID}_"*.log "$LOG_ROOT/debug/plog/plog-${PID}_"*.log)
shopt -u nullglob
if [ "${#plog_files[@]}" -gt 0 ]; then
  grep -iE "timeout|wait|hcclresult|CommTimeout|taskFailCallback|heartbeat|TS_ERROR|AICPU timeout" "${plog_files[@]}"
else
  echo "no plog found for PID $PID under $LOG_ROOT/{run,debug}/plog; skipping PID filter" >&2
fi

# 4. 模块前缀过滤（ERROR/FATAL 级 HCCL/RUNTIME/TDT；兼顾 ] 后有无空格，同时覆盖 run/plog 与 debug/plog）
shopt -s nullglob
all_plogs=("$LOG_ROOT/run/plog/"plog-*.log "$LOG_ROOT/debug/plog/"plog-*.log)
shopt -u nullglob
[ "${#all_plogs[@]}" -gt 0 ] || { echo "no plog files under $LOG_ROOT/{run,debug}/plog" >&2; exit 1; }
grep -hE "^\[(ERROR|FATAL)\][[:space:]]*(HCCL|RUNTIME|TDT)\(" "${all_plogs[@]}"

# 5. HCCL 三阶段多级检索（CANN 8.5.0+）
grep -HE "InitGroupStage|InitChannelStage|TaskExecStage" "${all_plogs[@]}"
```

**日志格式**：`[LEVEL] MODULE(pid,appname):YYYY-MM-DD-HH:MM:SS.ms.us [file:line][func][tid:pid] message`
- LEVEL ∈ {DEBUG, INFO, WARNING, ERROR, FATAL}
- MODULE 常见：HCCL / RUNTIME / TDT / OP / CCECPU / AICPU

## 输出解读

| 字段/关键字 | 含义 |
|------------|------|
| `appname` | 启动进程名（如 `run_comm_fix`），可关联 rank |
| `tid` | 线程 id，关联 pid，多 rank 场景区分 rank |
| `localRankId[0]` | comm config 中本 rank 编号 |
| `group[<ip%iface_..._rankSize>]` | 通信域信息，含本 rank 与对端 IP |
| `deviceLogicId` | 驱动逻辑 device id（plog 记录的驱动域逻辑编号，须结合 `ASCEND_RT_VISIBLE_DEVICES` 与 `npu-smi info` 映射到用户逻辑 id 与物理 chip id） |
| `CommTimeout` / `HCCL_EXEC_TIMEOUT` | 通信执行超时 |
| `HCCL_CONNECT_TIMEOUT` / `wait socket establish timeout` | 建链超时 |
| `AICPU timeout` / `TS_ERROR` / `ECC` / `SVID` / `HWERR` | device 侧硬件/AICPU 异常 |
| `error cqe` / `ROCE CQE ERROR` (EI0013) | RoCE 链路错误 |
| `HeartbeatAbnormal` | 集群心跳异常（CQE/STUCK/LOST） |

## 诊断决策

| plog 输出特征 | 指向 | 详见 |
|--------------|------|------|
| `CommTimeout` + `localRankId` | 锁定等待方 rank → [communication-timeout](../symptoms/communication-timeout.md) | [rank-localization](../method/rank-localization.md) |
| `waiting for rank M` / 某 rank 无 `notify` | 锁定缺失 rank → [single-rank-block](../symptoms/single-rank-block.md) | |
| `comm config of` 各 rank 间不一致 | collective 参数分歧 → [collective-param-mismatch](../symptoms/collective-param-mismatch.md) | |
| `[InitChannelStage][ParameterConflict]` / `CMD information ... check fail` | 参数一致性校验失败 (EI0005) → [collective-param-mismatch](../symptoms/collective-param-mismatch.md) | |
| `wait socket establish timeout` / `LINK_ERROR_INFO` | 建链超时 (EI0006) → [communication-timeout](../symptoms/communication-timeout.md) | |
| `stream` / `Wait` / `Record` / `deadlock` | stream/event 等待环 → [stream-event-cycle](../symptoms/stream-event-cycle.md) | [decision-trees](../method/decision-trees.md) |
| device-*/.log 含 `ECC`/`AICPU timeout`/`TS_ERROR` | device 侧硬件/AICPU 异常 → 触发 [msnpureport](msnpureport.md) + [msaicerr](msaicerr.md) | |
| `error cqe` / `Execution Error ROCE CQE` | RoCE 链路错误 (EI0013) → 触发 [hccn-tool](hccn-tool.md) | |

## 降级策略

若 `~/ascend/log/` 不存在或 plog 为空：
- 回退到应用级日志（stdout/stderr）中的 `timeout`/`waiting for rank` 关键字诊断
- 建议用户设置 `export ASCEND_GLOBAL_LOG_LEVEL=1` 后重跑采集（INFO 级即可见 comm config 与 HCCL_ENV）
- 在 `tool_evidence` 中标注 `skipped: plog_dir empty or not accessible`，不阻塞主诊断流程

## 扩展查阅

- 命令参数或语义本页未覆盖时，按知识访问说明检索固定版本知识卡并核对官方来源。
