# rank-group-mismatch（rank/group 不一致）

## 典型现象

**HCCL 层通信域协商故障**：rank-id 碰撞、worldSize 不一致、通信域成员唯一性被破坏，导致通信域初始化/协商失败。

**参与者映射表错配（执行阶段形态）**：表驱动配对映射构造——HcclBatchSendRecv 类批次的 send/recv item 表、对端基/peer 映射表、分组/路由表——中的目标 rank 错配。典型缺陷为**陈旧拓扑残留**：按旧 rank 数/旧对端基生成的表项未随当前通信域规模更新，目标 rank 超出当前域、或与对端视角无法配对（对端无对应视角项）。现象：通信域 init 本身成功，错在执行阶段消费的映射结构——调用点参数错误（`HCCL_E_PARA`）或无对端等待直至超时。

可观测信号：
- 返回码：`HCCL_E_PARA`(1，检测到 rank 重复 / item 表目标越界) 或 `HCCL_E_TIMEOUT`(9，等待缺失 rank 加入 / 无对端配对等待)
- 日志：`RANK-COLLISION: both procs claim rank=N`、`HcclCommInitRootInfoConfig` 失败、item 表/对端表打印目标 rank 超出通信域
- 现象：进程退出码非 0；协商阶段形态在通信域初始化即失败（未到达 collective 调用）；映射表形态 init 成功、在 collective/P2P 批次调用点失败或等待
- EI 故障码：EI0004（rankTable 加载失败）、EI0014（集群信息校验失败：IP 重复/IPv4-IPv6 混用/TLS 不一致）

**常见根因**：
- rank-id 碰撞：driver 硬编码 `hccl_rank = 0`，两进程都报到 rank=0 槽位，rank=1 缺失
- worldSize 不一致：各 rank 的 worldSize 配置不同
- device-rank 映射错配（注：CANN 9.0.0 的 HcclCommInitRootInfoConfig 显式接收 rank 参数，**不校验** device-rank 对齐）
- 通信域成员唯一性破坏：root_info 广播模式下成员重复
- 参与者映射表残留：对端基/peer 映射/批次 item 表按旧拓扑（旧 rank 数）生成，目标超出当前通信域或跨 rank 无法配对

## 调用工具

1. **[plog](../tools/plog.md)**：grep `HcclCommInit|comm init|RANK-COLLISION|rank`，定位通信域初始化失败点
2. 应用日志：检查各 rank 进程的 RANK/RANK_SIZE/worldSize 环境变量与 rank_table 配置

## 决策逻辑

详见 [decision-trees.md](../method/decision-trees.md) 树 A 建链阶段分支。核心判定：

1. 超时/失败发生在通信域初始化阶段（HcclCommInitRootInfoConfig）？
   - 是 → 本类（区别于 collective 执行阶段超时）
2. 各 rank 的 rank id 唯一？
   - 两进程同一 rank id → rank-id 碰撞 → 本类
3. worldSize 与实际进程数一致？
   - 不一致 → 本类
4. rank_table 配置正确（device_id/device_ip/rank_id 映射）？
   - IP 重复/IPv4-IPv6 混用/TLS 不一致 → EI0014 集群信息校验失败
5. 通信域 init 成功，但调用点目标/对端参数错配？
   - 错配值位于**表驱动映射构造条目**（批次 item 表/对端基/peer 映射表——目标超出当前域或对端无配对视角项）→ 本类（参与者映射表错配型）
   - 错配值为**调用点标量实参** → 按载体分派：集合调用（root/srcRank）→ [collective-param-mismatch](collective-param-mismatch.md)；P2P 点对点（dstRank/srcRank）→ [comm-operator-anomaly](comm-operator-anomaly.md)（寻址域违反）——见 INDEX B1「值类参数核对次序」第 3 条

**排除近似类型**：
- 与 [communication-timeout](communication-timeout.md)：本类在**通信域初始化阶段**失败（建链前/中）——协商形态；或 init 成功后在调用点因**映射结构错配**失败/等待——映射表形态；communication-timeout 可能在执行阶段无结构错配地超时
- 与 [single-rank-block](single-rank-block.md)：本类是 rank 身份/组配置/参与者映射结构错误（结构性）；single-rank-block 是 rank 行为阻塞（运行时）
- 与 [collective-param-mismatch](collective-param-mismatch.md)：错配目标位于**表驱动参与者映射构造**（批次 item 表/对端基/peer 映射表——跨 rank 配对映射的结构性数据，对端无对应视角项）→ 本类（映射表错配型）；位于**集合调用点标量寻址实参**（root/srcRank 等）→ collective-param-mismatch（配对错配型）——按 INDEX B1「值类参数核对次序」第 3 条载体分派

## 定位方法

引用 [rank-localization.md](../method/rank-localization.md)：
- 核对各 rank 进程的 RANK 环境变量与 driver 内 hccl_rank 派生逻辑
- 检查 rank_table（`$RANK_TABLE_FILE`）：server_count、device[]（device_id/device_ip/rank_id）
- plog 建链对端信息：`Transport init error! createLink para:`（localUserrank/remoteUserrank）

## 修复方向

→ [fix/INDEX.md#rank-group-mismatch](../fix/INDEX.md#rank-group-mismatch)
- `communication-param-fix`：hccl_rank 按环境变量 RANK 正确派生（`uint32_t hccl_rank = static_cast<uint32_t>(rank);`）
- 核对 rank_table / RANK_ID / RANK_SIZE / worldSize 一致性
- 映射表错配型：对端基/peer 映射/批次 item 表随当前通信域规模重建（目标归当前域内契约对端，恢复跨 rank 对称配对）
- 多进程端口冲突（EI0019）：`export HCCL_NPU_SOCKET_PORT_RANGE="auto"`
- 集群信息校验失败（EI0014）：修正 rank_table 中 IP 重复 / IPv4-IPv6 混用 / TLS 开关不一致（`hccn_tool -i N -tls -g | grep switch`）

## 验证要点

- 修复后通信域初始化（HcclCommInitRootInfoConfig）成功返回（无 HCCL_E_PARA/TIMEOUT）
- 各 rank 进程的 RANK 环境变量与通信域 rank 唯一性契约一致
- 公开数值验证或独立参考核对通过（HcclAllReduce 结果正确）

## 交叉引用

- 易混淆：[communication-timeout](communication-timeout.md)（执行阶段超时）、[single-rank-block](single-rank-block.md)（运行时阻塞）
- 关联：A3 超节点逻辑超节点 ID 误配（不同物理超节点配成同一逻辑超节点）→ vnic 互等超时
