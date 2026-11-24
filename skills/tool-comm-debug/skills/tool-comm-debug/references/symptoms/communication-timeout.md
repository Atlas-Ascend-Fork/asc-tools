# communication-timeout（通信超时）

## 典型现象

**HCCL 层故障**：卡间 collective 算子执行同步等待超时（HCCL_EXEC_TIMEOUT）或建链超时（HCCL_CONNECT_TIMEOUT）。

可观测信号：
- 应用日志：`watchdog timeout`、`timeout`、`waiting for rank`、`got signal`、`ERR99999`
- plog：`CommTimeout`、`wait timeout`、`HCCL_EXEC_TIMEOUT`、`HCCL_CONNECT_TIMEOUT`、`AICPU timeout`
- 返回码：`HCCL_E_TIMEOUT`(9)
- EI 故障码：EI0002（执行超时）、EI0006（建链超时）
- 退出码：非 0（被 watchdog/超时终止）

**阶段区分**：
- 通信前（建链超时）：`init_process_group` 前后、plog 含 socket/connect 失败、EI0006
- 通信中（执行超时）：collective 调用后、plog 含 `CommTimeout` + 算子类型、EI0002
- 通信后：算子 `done` 后再异常

**主型边界（与 single-rank-block 共存时）**：等待端出现 watchdog/hcclExecTimeOut/HCCL_E_TIMEOUT 超时信号时，若对端形态为崩溃退出（abort/core dumped）、跳过 collective 但显式销毁通信域后退出（判据见下）、collective 前过早销毁 comm/stream、建链期 device 使用错误（同卡争抢），则**主型为本类**——对端缺席仅作为根因记录，不据此改判 single-rank-block（判据详见 [INDEX.md](../INDEX.md) 边界规则 B1 与本文决策逻辑第 2 步）。注意："跳过 + 显式销毁"须核实**显式销毁调用证据**（该 rank 退出路径调用通信域销毁接口，plog 有销毁调用条目）；仅有进程退出自动回收产生的清理日志而**无**销毁调用证据时，属 single-rank-block 的"绕过通信域销毁直接退出"形态，不判本类（核验方法见 INDEX 边界规则 B1 之注）。

## 调用工具

调用顺序（证据增强、可降级）：
1. **[plog](../tools/plog.md)**（首选）：重跑取 plog，grep `timeout|wait|CommTimeout|HCCL_ENV`，定位等待方 rank 与等待点
2. **[hccn-tool](../tools/hccn-tool.md)**（链路排查）：超时阈值接近参考范围上限时，验证链路 UP/Success、丢包增量
3. **[msnpureport](../tools/msnpureport.md)**（硬件排查）：plog device 日志含 AICPU/ECC 时，导出并核对故障时间窗内的 bbox 与 device 日志
4. **[msprof](../tools/msprof.md)**（耗时分解）：需量化各 rank 等待时长、结合跨 rank 时间线区分长等待方与晚到对端时采集 `--hccl=on`

## 决策逻辑

详见 [decision-trees.md](../method/decision-trees.md) 树 A。核心判定：

1. **判阶段**：建链超时（EI0006）vs 执行超时（EI0002）
2. **判到齐性**：所有 rank 是否都到达 collective 调用点？
   - 否 → 按 [INDEX.md](../INDEX.md) 边界规则 B1 依**对端形态**判主型：
     - 崩溃退出（abort/SIGABRT/core dumped）/ 跳过 collective 但**显式销毁通信域**后退出（退出路径调用通信域销毁接口且有销毁调用日志条目；仅凭退出期自动回收清理日志不算）/ collective 前过早销毁 comm/stream / **编排分歧**（各 rank 对同组**集合通信**的调用次数分歧或 collective 顺序相反——各自集合调用序列自洽，但某集合点双方调用错配〔次数多的一侧等待不存在的下一轮 / 第一集合点算子类型互异〕→ 集合点等待超时；注意 P2P 点对点调用〔Send/Recv〕配对缺失致某 rank 阻塞在集合调用之前的点对点等待点 → 不属编排分歧，判 single-rank-block P2P 配对阻塞型）→ **主型仍为本类**（对端缺席/错配为根因，等待端超时为主现象）
      - host 侧有限时长长阻塞（sleep/IO 阻塞后才到达）/ 死循环永不推进 / 绕过通信域销毁直接退出（无销毁调用证据即 return/exit，ACL 运行时级收尾不算通信域销毁；含资源失败被吞后的假成功 exit 0）/ 到达但调用退化未有效参与（退化值被入口拒绝或零元素 no-op）/ 会话句柄错配（切 device/未 Record 的 event Wait/错 comm 句柄且错域 init 成功、collective 在错域自洽完成 no-op）→ 转向 [single-rank-block](single-rank-block.md)；**错 comm 句柄且各 rank 通信域归属不一致、错配侧调用即时失败（nullptr→`HCCL_E_PTR`/额外域 init `comm name already exists` 失败后仍调用）→ 转向 [collective-param-mismatch](collective-param-mismatch.md)（comm 句柄跨域不一致型，优先判，见 B1 comm 句柄错配族判定次序）**
   - 建链阶段：init 失败/超时若根因为 device 使用错误（多 rank 同卡争抢/硬编码同卡）→ **主型为本类**（建链分支）；仅通信域参数不一致（rank/worldSize/group 配置分歧）才判 [rank-group-mismatch](rank-group-mismatch.md)
   - 是 → 继续判参数一致性与链路
3. **判参数一致性**：count/dataType/op/root 各 rank 一致？
   - 否 → 转向 [collective-param-mismatch](collective-param-mismatch.md)
4. **判链路/硬件（限于所测设备与采样时间窗口）**：在目标设备与覆盖故障时间窗的采样中，`hccn_tool` 是否出现 DOWN/丢包增量，或 `msnpureport` bbox 是否检出硬件错？
   - 是 → `anomaly_module` 指向链路或硬件，继续核对物理端口与设备日志
   - 否 → 仅说明当前查询与已采集报告窗口未见上述异常（仍需核对故障时间窗与采样完整性）；结合跨 rank 源码调用序列、退出状态、通信参数一致性与等待关系的正向证据，确定是否属 rank 参与不对等、编排不一致或其余通信超时原因

**排除近似类型**：
- 与 [single-rank-block](single-rank-block.md)：不按"missing_rank 是否有进程级阻塞"判主型；按对端形态判（INDEX 边界规则 B1）——崩溃退出/显式销毁通信域后跳过（须有销毁调用证据）/过早销毁通信资源/编排分歧（调用次数或顺序不一致）→ 本类为主型；host 长阻塞/死循环/绕过通信域销毁直接退出（无销毁调用证据；含假成功 exit 0）/退化参与/句柄错配（错域自洽 no-op）→ single-rank-block 为主型（本类可作表现层结论）；错 comm 句柄致各 rank 通信域归属不一致且错配侧即时失败（`HCCL_E_PTR`/额外域 init 失败）→ collective-param-mismatch（优先判）
- 与 [stream-event-cycle](stream-event-cycle.md)：有 wait timeout 报错 → 本类；无报错静默 hang → stream-event-cycle
- 与 [collective-param-mismatch](collective-param-mismatch.md)：plog 有参数分歧证据 → collective-param-mismatch

## 定位方法

引用 [rank-localization.md](../method/rank-localization.md)：
- **wait_point**：`[rank N] calling <op>, waiting for rank M` → `wait_point = "rank N -> <op>"`
- **root_rank**：发起并等待的 rank（plog `CommTimeout` 条目的 `localRankId`）
- **missing_rank**：未到达 collective 的 rank（"waiting for rank M" 中的 M；跨进程对比无 notify 的 rank）

**超时阈值参考**（按部署拓扑）：
- cross-card：30-120s
- cross-node：120-300s
- 若观测超时远小于参考下限 → 倾向"参与不对等/执行不一致"根因

## 修复方向

→ [fix/INDEX.md#communication-timeout](../fix/INDEX.md#communication-timeout)
- 分支 A（rank 参与不对等）：`sync-relation-fix`，确保所有 rank 到达 collective 调用点
- 分支 A'（编排不一致）：`sync-relation-fix`，统一各 rank 的调用次数与 collective 顺序（同一训练循环迭代数、同组 collective 的排列次序各 rank 一致）
- 分支 B（链路异常）：`communication-param-fix`，HCCL_IF_IP/网卡 PFC/路由修复
- 分支 C（硬件异常）：硬件排查建议
- 分支 D（业务大算子慢）：调大 HCCL_EXEC_TIMEOUT；**超时阈值本身各 rank 配置不一致（如单位换算错误、阈值分歧致失败不同步）时，统一各 rank 的 HCCL_TIMEOUT_MS/hcclExecTimeOut 配置**

## 交叉引用

- 易混淆：[single-rank-block](single-rank-block.md)（进程层阻塞；共存时主型归属见 INDEX 边界规则 B1）、[collective-param-mismatch](collective-param-mismatch.md)（参数分歧致超时）、[stream-event-cycle](stream-event-cycle.md)（runtime 层死锁，无报错）
