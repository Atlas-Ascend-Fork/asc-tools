# 日志关键词 → 异常类型映射

> plog / 应用日志关键词到异常类型的映射表，含正则模板。供 agent 从日志特征快速分流。

## 1. 关键词 → 异常类型映射表

| 关键词 / 正则模式 | 来源 | 候选异常类型（优先级） |
|------------------|------|----------------------|
| `watchdog timeout` | 应用日志 | **communication-timeout** |
| `waiting for rank` | 应用/plog | **communication-timeout** → single-rank-block |
| `wait timeout` / `CommTimeout` / `HCCL_EXEC_TIMEOUT` | plog | **communication-timeout** |
| `got signal` / `ERR99999` | 应用日志 | **communication-timeout**（兜底上报） |
| `NOT participating` / `sleeping` / `skipping` | 应用日志 | **single-rank-block** → communication-timeout |
| `stuck in infinite loop` | 应用日志 | **single-rank-block**（死循环型） |
| `HCCL_E_TIMEOUT` (返回码 9) | plog/driver | **communication-timeout**（建链/执行） |
| `HCCL_E_PARA` (返回码 1) | plog/driver | **collective-param-mismatch** / **rank-group-mismatch** / comm-operator-anomaly |
| `CMD information .* check fail` / `[InitChannelStage][ParameterConflict]` | plog (EI0005) | **collective-param-mismatch** |
| `RANK-COLLISION` / rank-id 碰撞 | driver 日志 | **rank-group-mismatch** |
| `HcclCommInitRootInfoConfig` 失败 | plog/driver | **rank-group-mismatch** → communication-timeout |
| `HCCL_CONNECT_TIMEOUT` / `wait socket establish timeout` / `LINK_ERROR_INFO` | plog (EI0006) | **communication-timeout**（建链阶段） |
| `count` / `dataType` / `op` / `root` 各 rank 不一致 | plog `comm config of` | **collective-param-mismatch** |
| `count.*OOB` / count 远超 buffer | driver 日志 | **comm-operator-anomaly**（API 契约违反） |
| `aclnn.*fail` / `161001` / `561000` / `561002` / `561003` | driver/plog | 候选 **comm-operator-anomaly**；先看失败阶段和详细错误，不把全部返回码归因于 kernel |
| `TS_ERROR` / `aicore error` | plog device | **comm-operator-anomaly** → 触发 msaicerr |
| `stream` / `StreamWaitEvent` / `Record` + `deadlock` | plog | **stream-event-cycle** |
| 双 stream 循环 Wait + 无报错 hang | plog/现象 | **stream-event-cycle**（runtime 层死锁） |
| `calcDoneEvent` / `event prime` / `Reset` + 阻塞 | plog | **sync-imbalance** |
| 计算与通信时间线未重叠 | msprof | **sync-imbalance**（过度串行化） |
| `AICPU timeout` | plog device | **communication-timeout**（device 侧） → 触发 msnpureport |
| `aicpu completion wait failed` / `completion wait.*failed`（host 等待 kernel 回发完成通知超时——AI CPU+TS 载体） | driver stderr | 算子为自研 **kernel 编排引擎**通信算子且参数一致/无缺席/无环 → **comm-engine-protocol-fault**（AI CPU+TS kernel 载体）；否则 communication-timeout |
| `vector core timeout` / 流同步返回超时错误码（507034 类，kernel 下发后 stream synchronize 失败；失败浮出耗时≈launch 级超时兜底值 + 收尾开销） | driver stderr / plog | 算子为自研 **kernel 编排引擎**通信算子·AIV/Vector Core 载体（host 加载下发 Ascend C kernel）且参数一致/无缺席/无环 → **comm-engine-protocol-fault**（kernel 载体·软同步位：flag 等待槽位与发布槽位两侧异表 → kernel 内死自旋）；否则 communication-timeout |
| 单侧 op 返回 `ret=0` 且耗时异常短（假成功）/ 同一算子第一次调用通过第二次超时 | driver/stdout | **comm-engine-protocol-fault**（原语类别错用·假成功 / 成功路径状态收口缺失·跨调用泄漏；配合跨 rank/跨调用日志关联） |
| `ECC` / `SVID` / `HWERR` | plog device | 硬件异常 → 触发 msnpureport + msaicerr |
| `error cqe` / `ROCE CQE ERROR` (EI0013) | plog | 链路异常 → 触发 hccn-tool |
| `HeartbeatAbnormal` / `Cluster Exception Location` | plog (TaskExecStage) | 集群心跳异常（CQE/STUCK/LOST） |
| `Execution_Error_SDMA` / `EI0012` | plog | SDMA 任务异常（链路/内存） |
| `Resource_Error_Insufficient_Device_Memory` / `EI0011` | plog | OOM |

## 2. 模块前缀过滤正则模板

```bash
# ERROR/FATAL 级 HCCL/RUNTIME/TDT 模块（兼顾 ] 后有无空格；仅对实际存在的 run/debug plog 执行）
shopt -s nullglob
plogs=(~/ascend/log/{run,debug}/plog/plog-*.log)
shopt -u nullglob
[ "${#plogs[@]}" -gt 0 ] || { echo "no plog files under ~/ascend/log/{run,debug}/plog" >&2; exit 1; }
grep -hE "^\[(ERROR|FATAL)\][[:space:]]*(HCCL|RUNTIME|TDT)\(" "${plogs[@]}"

# ERROR/FATAL 日志写入 debug/plog/，以下检索同时覆盖 run/plog/ 和 debug/plog/。
# HCCL 三阶段多级检索（CANN 8.5.0+，格式 [一级][二级]）
grep -HE "InitGroupStage|InitChannelStage|TaskExecStage" "${plogs[@]}"

# 通信异常特征关键词（超时/等待/状态/设备错误）
grep -iE "timeout|wait|hcclresult|CommTimeout|taskFailCallback|heartbeat|TS_ERROR|AICPU timeout" "${plogs[@]}"

# stream/event 等待链
grep -iE "stream|event|Wait|WaitNotify|wait monitor|wait subprocess|wait hccp|Record|deadlock" "${plogs[@]}"

# 通信结果/状态类
grep -iE "taskFailCallback|task_exception_handler|HeartBeat|UnRegisterRanks|SnapshotControl" "${plogs[@]}"

# HCCL 环境与拓扑配置
grep -iE "HCCL_ENV|HCCL_IF_IP|HCCL_SOCKET_IFNAME|comm config of|group\[|deviceLogicId" "${plogs[@]}"
```

## 3. HcclResult 错误码速查

| 错误码 | 名称 | 含义 | 指向 |
|--------|------|------|------|
| 0 | HCCL_SUCCESS | 成功 | — |
| 1 | HCCL_E_PARA | 参数错误 | collective-param-mismatch / rank-group-mismatch / comm-operator-anomaly |
| 4 | HCCL_E_INTERNAL | 内部错误（隐性不一致落地） | collective-param-mismatch |
| 8 | HCCL_E_SYSCALL | 系统调用失败（端口绑定等） | rank-group-mismatch (EI0019) |
| 9 | HCCL_E_TIMEOUT | 超时（通信类最常见） | communication-timeout |
| 11 | HCCL_E_TCP_CONNECT | tcp 建链失败 | communication-timeout（建链） |
| 12 | HCCL_E_ROCE_CONNECT | roce 建链失败 | communication-timeout（建链/链路） |
| 21 | HCCL_E_REMOTE | error cqe（对端错误） | 链路异常 → hccn-tool |
| 24 | HCCL_E_OOM | 显存不足 | 资源问题 (EI0011) |

## 4. EI 故障码（业务侧对外故障码）速查

| EI 码 | 场景 | 阶段/关键字 | 指向 |
|-------|------|------------|------|
| EI0002 | 算子执行超时 | TaskExecStage/Timeout | communication-timeout（HCCL_EXEC_TIMEOUT） |
| EI0003 | 入参校验失败 | TaskExecStage/InvalidArgument | comm-operator-anomaly |
| EI0005 | 参数一致性校验失败 | InitChannelStage/ParameterConflict | collective-param-mismatch |
| EI0006 | 参数面建链超时 | InitChannelStage/Timeout | communication-timeout（HCCL_CONNECT_TIMEOUT） |
| EI0011 | QP 内存申请失败（OOM） | Resource_Error_Insufficient_Device_Memory | 资源问题 |
| EI0012 | SDMA 任务异常 | Execution_Error_SDMA | 链路/内存 |
| EI0013 | ROCE CQE ERROR | error cqe status | 链路异常 → hccn-tool |
| EI0019 | 端口绑定失败 | socket type | rank-group-mismatch / 多进程端口冲突 |

## 5. 层级判别口诀

诊断分叉的第一步——**先判故障层**：
- 有 `wait timeout` / HCCL 错误码 → **HCCL 层**（communication-timeout / collective-param-mismatch）
- 无报错但 hang，且 host 线程停在 `aclrtSynchronizeStream` → **runtime 层**（stream-event-cycle）
- 多 rank 中某 rank 卡在非通信代码（死循环/sleep/提前退出）→ **进程层**（single-rank-block）

详见 [decision-trees.md](decision-trees.md)。
