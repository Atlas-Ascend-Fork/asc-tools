# Rank 定位方法

> 如何从 plog 的 pid / deviceLogicId / localRankId / comm config 定位异常 rank；如何关联对端 rank 构建等待关系。

## 1. plog 字段 → rank 映射

plog 日志格式：`[LEVEL] MODULE(pid,appname):<timestamp> [file:line][func][tid:pid] message`

| plog 字段 | 定位作用 |
|-----------|---------|
| `pid` | 进程 id，每个 rank 进程一个 pid，plog 文件名含 pid：`plog-<pid>_<ts>.log` |
| `appname` | 启动进程名（如 `run_comm_fix`），同业务各 rank 通常同名，靠 pid 区分 |
| `tid` | 线程 id，关联 pid，多线程场景区分 |
| `localRankId[0]` | comm config 中本 rank 编号（**最直接**） |
| `group[<ip%iface_..._rankSize>]` | 通信域信息，含本 rank IP + rankSize + 对端 IP |
| `deviceLogicId` | plog `comm config` 中的驱动逻辑设备编号（需结合环境可见性与拓扑映射关联到物理 chip id 与用户可见逻辑 device id，不得直接混用） |

## 2. 定位异常 rank 的步骤

### 步骤 1：确定哪些 rank 进程在跑

```bash
# 列出所有 plog 文件（按时间排序），每个文件对应一个进程
ls -t ~/ascend/log/run/plog/
ls -t ~/ascend/log/debug/plog/
# 输出形如：plog-<pid1>_<ts>.log, plog-<pid2>_<ts>.log ...
```

### 步骤 2：提取每个 rank 的 comm config（拿到 localRankId 与对端）

```bash
shopt -s nullglob
plogs=(~/ascend/log/{run,debug}/plog/plog-*.log)
shopt -u nullglob
[ "${#plogs[@]}" -gt 0 ] || { echo "no plog files under ~/ascend/log/{run,debug}/plog" >&2; exit 1; }
grep -iE "comm config of|localRankId|group\[|deviceLogicId" "${plogs[@]}"
# 输出示例：comm config of group[192.168.1.1%eth0_..._rankSize2] localRankId[0] deviceLogicId[4]
```

- `localRankId[0]` → 本 rank 是 rank 0
- `group[...]` 中 `rankSize2` → 通信域共 2 个 rank
- IP 字段 → 本 rank 与对端 rank 的 IP
- `deviceLogicId[4]` → 驱动逻辑 device id 为 `4`（在 `ASCEND_RT_VISIBLE_DEVICES=4,6` 的单 die 示例环境中，对应物理 chip `4` 与进程内用户可见逻辑 device `0`；具体对应以当前 `npu-smi info` 与运行时配置为准）

### 步骤 3：定位等待方 rank（root_rank）

```bash
# 找 CommTimeout / wait timeout 条目，含 localRankId 即等待方
shopt -s nullglob
plogs=(~/ascend/log/{run,debug}/plog/plog-*.log)
shopt -u nullglob
[ "${#plogs[@]}" -gt 0 ] || { echo "no plog files under ~/ascend/log/{run,debug}/plog" >&2; exit 1; }
grep -iE "CommTimeout|wait timeout|HCCL_EXEC_TIMEOUT" "${plogs[@]}"
# 输出示例：OpName:[CommTimeout_0] ... localRankId[0] → 等待方是 rank 0
```

→ `root_rank = rank 0`（发起并等待的 rank）

### 步骤 4：定位缺失/阻塞 rank（missing_rank）

**方法 A：跨进程对比（推荐）**
同一 group 下两个 rank 的 plog 对照看：
- 谁先到 wait → 等待方（root_rank）
- 谁没发 notify / 没到达 collective 调用点 → 缺失方（missing_rank）

```bash
# 对比两个 rank 的算子调用日志（先设置并校验两个纯数字 PID；HCCL_ENTRY_LOG_ENABLE=1 时更清晰）
LOG_ROOT="$HOME/ascend/log"
PID1="12345"  # 替换为 rank 0 对应纯数字 PID
PID2="12346"  # 替换为 rank 1 对应纯数字 PID
case "$PID1:$PID2" in
  ''|*[!0-9:]*|*:*[!0-9]*|:*|*:) echo "invalid PID: $PID1 / $PID2" >&2; exit 1 ;;
esac
shopt -s nullglob
plog1=("$LOG_ROOT/run/plog/plog-${PID1}_"*.log "$LOG_ROOT/debug/plog/plog-${PID1}_"*.log)
plog2=("$LOG_ROOT/run/plog/plog-${PID2}_"*.log "$LOG_ROOT/debug/plog/plog-${PID2}_"*.log)
shopt -u nullglob
[ "${#plog1[@]}" -gt 0 ] || { echo "no plog found for PID $PID1 under $LOG_ROOT/{run,debug}/plog; fallback to 方法 B（应用日志直读）" >&2; exit 1; }
[ "${#plog2[@]}" -gt 0 ] || { echo "no plog found for PID $PID2 under $LOG_ROOT/{run,debug}/plog; fallback to 方法 B（应用日志直读）" >&2; exit 1; }
grep -HiE "Entry-HcclAllReduce|Entry-HcclBroadcast|notify" "${plog1[@]}" "${plog2[@]}"
```

**方法 B：应用日志直读**
- `[rank 1] NOT participating in collective, sleeping 20s` → missing_rank = rank 1
- `waiting for rank 1` → rank 1 即缺失方
- 某 rank 缺少 `<op> done` 完成行（其他 rank 均有）→ 该 rank 缺失

### 步骤 5：关联对端 rank

从 comm config 的 `group[<ip%iface_...>]` 字段提取通信域拓扑：
- 本 rank IP + 对端 IP → 构建两两关系
- cross-node 场景：核对对端 IP 路由可达性（配合 [hccn-tool](../tools/hccn-tool.md) `-route -g`）

## 3. chip id ↔ 逻辑 device id 映射（关键易错点）

不同工具与日志字段使用三类设备编号域，混用会导致 "invalid device id" 或指向错误设备：

- **用户可见逻辑 device id**：受 `ASCEND_RT_VISIBLE_DEVICES` 裁剪后的进程内逻辑编号（示例 `0, 1`），供 `aclrtSetDevice` 及运行时工具使用（容器或不同版本下的具体参数语义仍以现场 `--help` 为准）。
- **驱动逻辑 device id**：plog `comm config` 中的 `deviceLogicId`（示例 `4, 6`）及部分底层物理内存访问权限接口使用的驱动域逻辑编号。
- **物理 chip id / Phy-ID**：硬件拓扑与网卡链路层的物理芯片编号（示例 `4, 6`；双 die 机型如 Atlas A3 须通过 `npu-smi info` 区分 NPU 模组号、`Chip` 列与 `hccn_tool` 所需的 `Phy-ID`）。

在 `ASCEND_RT_VISIBLE_DEVICES=4,6` 的典型双卡示例环境下，三类编号与各工具的对应关系如下（以当前 `npu-smi info -t topo -m` 与现场工具 `--help` 实测为准）：

| 工具 / 日志字段 | 使用的编号域 | 示例取值（`ASCEND_RT_VISIBLE_DEVICES=4,6`） |
|-----------------|--------------|----------------------------------------------|
| [hccn-tool](../tools/hccn-tool.md) (`-i`) | **物理 chip id**（Atlas A3 为 `Phy-ID`） | 4, 6 |
| `npu-smi info` | **物理 chip id** / 拓扑编号 | 4, 6 |
| [hccl-test](../tools/hccl-test.md) (`HCCL_TEST_USE_DEVS`) | **物理 chip id** | 4, 6 |
| [plog](../tools/plog.md) (`deviceLogicId`) | **驱动逻辑 device id**（本例与主机物理槽位 `4, 6` 数值相同，语义属驱动逻辑域） | 4, 6 |
| [msnpureport](../tools/msnpureport.md) (`-d`) | **用户可见逻辑 device id**（具体容器映射以 `--help` 为准） | 0, 1 |
| [msprof](../tools/msprof.md) (`--sys-devices`) | **用户可见逻辑 device id** | 0, 1 |
| [msaicerr](../tools/msaicerr.md) (`-dev`) | **用户可见逻辑 device id** | 0, 1 |

**显式映射示例**（通过 `npu-smi info -t topo -m` 与当前运行环境变量核对）：物理 chip `4` / 驱动逻辑 device `4` → 用户可见逻辑 device `0`；物理 chip `6` / 驱动逻辑 device `6` → 用户可见逻辑 device `1`。

## 4. HCCL 建链根节点定位（cross-node 场景）

建链超时（EI0006）会 rank 间级联传递。HCCL 提供 `LINK_ERROR_INFO` 表与 `CONNECT TIMEOUT DETECT RESULT` 定位真正的失败连接：

```bash
shopt -s nullglob
plogs=(~/ascend/log/{run,debug}/plog/plog-*.log)
shopt -u nullglob
[ "${#plogs[@]}" -gt 0 ] || { echo "no plog files under ~/ascend/log/{run,debug}/plog" >&2; exit 1; }
# 建链超时检索
grep -H "wait socket establish timeout" "${plogs[@]}"
# LINK_ERROR_INFO 表（字段：dest_ip/dest_port/src_ip/src_port/MyRole/Status/TlsStatus）
grep -H "LINK_ERROR_INFO" "${plogs[@]}"
# 建链对端信息（localUserrank/localIpAddr/remoteUserrank/remoteIpAddr/linkMode/isUsedRdma）
grep -H "Transport init error! createLink para:" "${plogs[@]}"
```

## 5. 心跳定位（TaskExecStage 集群异常）

集群心跳机制提供单点故障广播（CQE > STUCK > LOST 优先级）：

```bash
shopt -s nullglob
plogs=(~/ascend/log/{run,debug}/plog/plog-*.log)
shopt -u nullglob
[ "${#plogs[@]}" -gt 0 ] || { echo "no plog files under ~/ascend/log/{run,debug}/plog" >&2; exit 1; }
# 心跳异常定位结论
grep -H "HeartbeatAbnormal" "${plogs[@]}"
# 定位结论日志
grep -H "Cluster Exception Location" "${plogs[@]}"
# 格式：[TaskExecStage][HeartbeatAbnormal]Cluster Exception Location[IP/ID], Arrival Time, Discoverer[IP/ID], ExceptionType, Possible Reason
```

- `Error cqe Occurred` → 网络问题（QPN 映射远端 IP）
- `Stuck Occurred` → 进程卡死
- `Heartbeat Lost Occurred` → 进程退出

## 6. 多流场景的 rank → stream → event 关联

stream/event 等待环诊断时，需构建按操作序号展开的 rank → `(stream, op_seq)` 操作级依赖图（避免将整条 stream 合并为单节点而误伤分阶段合法同步）：
- 按 `tid`/`pid` 关联 rank，并在每条 stream 内按主机提交先后建立程序序有向边 `(S, k) → (S, k+1)`
- 按具体 event 实例或轮次代次建立跨流完成依赖有向边（`Record(E_i)` 所在操作节点 → `Wait(E_i)` 所在操作节点），并结合 `Reset` 与复用状态核对 `Wait` 绑定的 `Record` 是否可达
- 在操作级有向图上检测是否存在不可推进的循环等待环（如两流各自先 `Wait` 对方排在本阻塞点之后的 `Record`）→ 操作级环或不可达 `Record` = 死锁根因（详见 [decision-trees.md](decision-trees.md)）
