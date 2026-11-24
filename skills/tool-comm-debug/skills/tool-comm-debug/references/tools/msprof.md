# msprof — HCCL 通信 Profiling

> 采集 HCCL 通信算子耗时分解（wait/notify/transport/notify_wait 子阶段），量化各 rank 等待与传输时长，结合跨 rank 调用时间线区分长等待方（waiting rank）、晚到/偏斜对端与链路带宽瓶颈。

## 工具定位

CANN 性能 profiler 主入口（`$ASCEND_HOME_PATH/bin/msprof`）。对通信异常诊断的核心价值：用 `--hccl=on` 量化通信耗时，区分"等待环/同步失衡"与"链路带宽瓶颈"。

**device id 约定**：`--sys-devices` 使用**用户可见逻辑 device id**（示例 `0,1`），按当前环境 `--help` 与设备映射核对，避免与物理 chip id 或驱动逻辑号混淆。

## 调用命令

```bash
case "${ASCEND_HOME_PATH:-}" in
  /*) ;;
  *) echo "require non-empty absolute ASCEND_HOME_PATH" >&2; exit 1 ;;
esac
SDK_REAL=$(realpath -m -- "$ASCEND_HOME_PATH") || exit 1
[ "$SDK_REAL" != "/" ] && [ -d "$SDK_REAL" ] || { echo "invalid ASCEND_HOME_PATH" >&2; exit 1; }
MSPROF_BIN="${SDK_REAL}/bin/msprof"
[ -x "$MSPROF_BIN" ] || { echo "missing executable: $MSPROF_BIN" >&2; exit 1; }

# 1. 采集 HCCL 通信 Profiling（用户可见逻辑 id 0,1；实际运行按目标应用追加参数）
"$MSPROF_BIN" --hccl=on --task-time=on --aicpu=on \
       --output=./prof_out --duration=60 --sys-devices=0,1 \
       ./my_train_app

# 2. 同时采集 NIC/ROCE 与 HCCS 带宽（定位链路瓶颈）
"$MSPROF_BIN" --hccl=on \
       --sys-io-profiling=on --sys-interconnection-profiling=on \
       --output=./prof_out --duration=60 ./my_train_app

# 3. 采集后自动分析（生成 communication matrix）
"$MSPROF_BIN" --hccl=on --output=./prof_out --analyze=on \
       --rule=communication,communication_matrix ./my_train_app
```

**关键参数**：
| 参数 | 说明 |
|------|------|
| `--hccl=on` | 采集 HCCL profiling（通信诊断必开，默认 off） |
| `--task-time=on` | task 耗时（默认 on） |
| `--aicpu=on` | AICPU 耗时（看通信算子是否被 AICPU 任务阻塞） |
| `--sys-io-profiling=on` | NIC/ROCE 带宽采集 |
| `--sys-interconnection-profiling=on` | PCIe/HCCS 链路带宽采集 |
| `--sys-devices=0,1` | 指定 device 范围（逻辑 id） |
| `--duration=<s>` | 采集时长（秒） |
| `--delay=<s>` | 延迟启动（避开 init 阶段） |
| `--rule=communication,communication_matrix` | 分析规则 |

## 输出解读

输出目录 `prof_out/PROF_xxx/`：
- `device_x/`：各 device 的 trace、aicpu、hccl 数据
- `mindstudio_profiler_output/communication_matrix.csv`：各 rank 各 collective 算子耗时矩阵
- `SUMMARY/`：通信算子耗时、wait 阶段分解、rank 耗时矩阵

**关键指标**：
- hccl trace 中 `wait`/`wait_notify` 阶段耗时 → 等待时长
- communication_matrix.csv 中各 rank allreduce/allgather 耗时差异 → 负载偏斜

## 诊断决策

| msprof 输出特征 | 指向 | 详见 |
|---------------|------|------|
| 某 rank 的 wait 阶段耗时异常长 | 定位出该集合点的**长等待方（waiting rank）**；需跨 rank 对齐同通信域、同调用序号的进入时间、前序计算完成时间、退出状态及 `notify`/`transport` 关系，再确定晚到或缺失对端（`missing_rank`）→ [communication-timeout](../symptoms/communication-timeout.md) | [rank-localization.md](../method/rank-localization.md) |
| 各 rank 耗时差异大（负载不均） | collective 负载偏斜，修复偏向通信参数/算法均衡 | |
| wait 阶段短但整体慢 + NIC/HCCS 带宽打满 | 链路带宽瓶颈 → 触发 [hccn-tool](hccn-tool.md) 带宽验证 | |
| AICPU 占用高抢占通信算子 | 通信算子被 AICPU 任务阻塞 → [sync-imbalance](../symptoms/sync-imbalance.md) | |
| stream 时间线出现长空闲/等待段且不结束 | runtime 层等待环/死锁 → [stream-event-cycle](../symptoms/stream-event-cycle.md) | |
| 计算与通信时间线未重叠（退化为串行） | 过度串行化（错等）→ [sync-imbalance](../symptoms/sync-imbalance.md) | |

## 降级策略

- 非 root 跑 msprof 报 `Operation not permitted`（ACL 初始化受限）：在 `tool_evidence` 标注 `skipped: requires acl/root`，降级为基于 [plog](plog.md) 的等待点定位
- 执行采集时保留命令退出码、标准错误输出与 `PROF_xxx` 有效产物检查结果；目标应用实际调起 `aclInit` 并执行设备任务后才会生成对应通信 trace
- 若无法采集，回退到 [plog](plog.md) 的 `CommTimeout`/`wait` 关键词，并按 [rank-localization.md](../method/rank-localization.md) 对比各 rank 同序号算子进入与完成日志区分长等待方与晚到/缺失对端

## 扩展查阅

- 当本页未覆盖所需的采集方法或命令参数、或选项语义不明时，查阅知识库中的 profiling 卡片和现场 msprof --help。
- 命令参数或语义本页未覆盖时，按知识访问说明检索固定版本知识卡并核对官方来源。
