# 多卡通信异常诊断知识树索引

根据现象和日志选择候选异常类型，再按需查阅对应的症状、工具、方法和修复章节。

## 一、症状分流表

下表按可观测关键词和信号列出候选类型，同一行内按排查优先级排列。

| 关键词/信号 | 候选异常类型（优先级） |
|------------|---------------------|
| `timeout` / `watchdog timeout` | [communication-timeout](symptoms/communication-timeout.md) → single-rank-block |
| `waiting for rank` | [communication-timeout](symptoms/communication-timeout.md) → [single-rank-block](symptoms/single-rank-block.md) |
| `wait timeout` / `CommTimeout` / `HCCL_EXEC_TIMEOUT` | [communication-timeout](symptoms/communication-timeout.md) |
| 各 rank 调用次数 / 同组 collective 顺序不一致（编排分歧） | [communication-timeout](symptoms/communication-timeout.md)（编排不一致型，B1） |
| 单 rank 传零元素 / 空值 / 退化参数被入口检查拒绝 | [single-rank-block](symptoms/single-rank-block.md)（调用退化型，B1 值类参数核对次序） |
| `NOT participating` / `sleeping` / `skipping` | 依边界规则 **B1** 判主型：[communication-timeout](symptoms/communication-timeout.md) ↔ [single-rank-block](symptoms/single-rank-block.md) |
| `abort` / 进程崩溃 / segfault | 伴随其余 rank 通信等待超时 → [communication-timeout](symptoms/communication-timeout.md)（**B1**）；算子执行内部崩溃/无通信等待 → [comm-operator-anomaly](symptoms/comm-operator-anomaly.md) → rank-group-mismatch |
| `mismatch` / `inconsistent` / `CMD information.*check fail` | [collective-param-mismatch](symptoms/collective-param-mismatch.md) |
| count/dataType/op/root 各 rank 不一致 | [collective-param-mismatch](symptoms/collective-param-mismatch.md)；按 B1「值类参数核对次序」分型：数值退化值（count=0/空）→ single-rank-block 调用退化型；**值域违反（溢出/超界非法值——近类型极值、超出 API/缓冲契约域，单侧即不可能合法）→ comm-operator-anomaly（参数域契约违反型）**；目标/寻址类越界按**载体**分派（集合调用点标量 → collective-param-mismatch 配对错配型；P2P 点对点标量 → comm-operator-anomaly 寻址域违反；表驱动映射构造〔批次 item 表/对端基〕→ rank-group-mismatch 映射表错配型）；分块类参数（AlltoAll 块大小）先按单 rank 视角核对算子语义约束（分块之和 = **全局输入张量第 0 维**）——任一 rank 违反 → comm-operator-anomaly |
| P2P 目标参数越界 / 批次 item 表对端错配（`remoteRank`/`dstRank` 指向不存在对端、对端无配对视角项） | [comm-operator-anomaly](symptoms/comm-operator-anomaly.md)（点对点调用点标量目标——寻址域违反）→ [rank-group-mismatch](symptoms/rank-group-mismatch.md)（表驱动映射构造条目错配）；按 B1「值类参数核对次序」第 2/3 条载体分派 |
| `ptr [comm] is nullptr`/`HCCL_E_PTR` / `comm name already exists` / 各 rank collective 调用所用 comm 句柄不同通信域 | [collective-param-mismatch](symptoms/collective-param-mismatch.md)（comm 句柄跨域不一致型；错配域 init 成功且 collective 在错域自洽完成〔no-op〕→ single-rank-block，见 B1） |
| `cycle` / `wait event` / 双 stream 循环 Wait + 无报错 hang；跨 stream 依赖边缺失 / event 未 Reset 复用 + 数据竞争或结果错误（非 hang） | [stream-event-cycle](symptoms/stream-event-cycle.md)（**B2**） |
| `imbalance` / 计算流通信流未重叠 / event prime+reset 单向永久阻塞 / SynchronizeStream 过度串行化 | [sync-imbalance](symptoms/sync-imbalance.md)（**B2**；依赖边完全缺失归 stream-event-cycle） |
| `ECC` / `SVID` / `HWERR` | 硬件异常 → [comm-operator-anomaly](symptoms/comm-operator-anomaly.md)（触发 msaicerr） |
| `AICPU timeout` | [communication-timeout](symptoms/communication-timeout.md)（device 侧）→ 触发 msnpureport |
| `TS_ERROR` | [comm-operator-anomaly](symptoms/comm-operator-anomaly.md) → 触发 msaicerr |
| `HcclResult` / HCCL_E_TIMEOUT(9) | [communication-timeout](symptoms/communication-timeout.md) |
| 静默停滞（stderr 空、无任何超时/原语报错行）+ 编排层超时兜底截断非 0 退出 + 算子为自研通信算子·host 编排引擎（主机端单边写 + 分离式事件通知原语组合，看机制类别不看原语库符号名） | [comm-engine-protocol-fault](symptoms/comm-engine-protocol-fault.md)（通知链路断配方——通知发布义务缺口/事件编号两侧异表；等待原语不按超时参数报错属已观测行为，勿以"无超时报错"排除） |
| 通信正常完成 + 无任何报错 + 数据级校验失败（远端来源段整段不符/零值）+ 自研通信算子（host 或 kernel 编排引擎均可） | [comm-engine-protocol-fault](symptoms/comm-engine-protocol-fault.md)（消息语义坏配方——写目的槽位映射错/拼装读源缓冲口径混用/读长分段口径错；与 [comm-operator-anomaly](symptoms/comm-operator-anomaly.md) 分界：无 API 报错即本家族） |
| host 完成等待超时失败行（`completion wait failed` 类——等待 kernel 回发完成通知）+ 算子为自研 **kernel 编排引擎**通信算子（工程含设备侧 kernel 数据面编排源码）+ collective 参数一致/无 rank 缺席/无等待环且 kernel 侧无错误行 | [comm-engine-protocol-fault](symptoms/comm-engine-protocol-fault.md)（通知链路断配方·kernel 载体位——kernel 内等待索引异表/数据面任务提交句柄错用/成功路径状态收口缺失；对称或非对称滞留；与 [communication-timeout](symptoms/communication-timeout.md) 分界：communication-timeout 第一跳排查项全落空 + 失败锚点在 kernel 回发等待 → 本家族） |
| 单侧 op 返回异常快"成功"（本端数据校验通过）+ 对端等待超时失败（症状与根因分居两侧）+ 自研 kernel 编排引擎算子 | [comm-engine-protocol-fault](symptoms/comm-engine-protocol-fault.md)（通知原语类别错用·假成功形态——数据面就绪通知误发到 host 完成等待所在控制通道，对端等不到 + 本端完成等待被误触发） |
| 同一算子/同一通信标签第一次调用全部通过 + 第二次调用对称等待超时（跨调用状态泄漏签名） | [comm-engine-protocol-fault](symptoms/comm-engine-protocol-fault.md)（成功路径引擎状态收口缺失——按标签存续的批量/会话状态未收口，跨调用浮出；"错误反而无害、成功反而埋雷"） |
| 通信/握手/返回码全正常 + op 返回耗时异常短 + 输出偶发全零/部分拼装（预置缓冲未被覆盖，间歇性） | [comm-engine-protocol-fault](symptoms/comm-engine-protocol-fault.md)（消息语义坏配方·完成回发时序提前——host 回读先于设备侧数据面任务落地竞态；与"算子未执行/未部署"以执行链路自证证据区分） |
| 接收侧输出前段精确 + 自某元素界起整段保持预置值（如预置零）+ 自研 kernel 编排引擎算子 | [comm-engine-protocol-fault](symptoms/comm-engine-protocol-fault.md)（消息语义坏配方·单边读长度/分段偏移口径错——读长误用固定类型尺寸或分段按错误视角取值） |
| 对称交换类算子两 rank 输出逐字节相同而期望互异 + 错值为**合法原值**（互补分段错位） | [comm-engine-protocol-fault](symptoms/comm-engine-protocol-fault.md)（消息语义坏配方·保留/发送分段倒置——上送分段偏移误用自视角索引；与归约切片归属错〔错值为**和**〕按错值类别区分） |
| kernel 下发后流同步超时失败（`vector core timeout` / 流同步返回超时错误码 507034 类；失败浮出耗时与 launch 级超时兜底值同量级）+ 算子为自研 **kernel 编排引擎**通信算子·AIV/Vector Core 载体（host 加载下发 Ascend C kernel 至 Vector Core，工程含设备侧 kernel 数据面编排源码）+ collective 参数一致/无 rank 缺席/无等待环 | [comm-engine-protocol-fault](symptoms/comm-engine-protocol-fault.md)（通知链路断配方·kernel 载体 AIV/软同步位——软同步 flag 等待轮询槽位与发布方发布槽位两侧异位致 kernel 内死自旋、launch 级超时兜底截断，对称或非对称；与 [communication-timeout](symptoms/communication-timeout.md) 分界：communication-timeout 第一跳排查项全落空 + 失败锚点在 kernel 下发后的流同步等待 + 有超时失败行不排除本家族〔控制面自报非原语错误码〕→ 本家族） |
| 归约/切片类自研算子通信正常完成无报错 + 两 rank 输出相同（或非互异）而期望各不同 + 错值为聚合**"和"**（拉模式归约读：全量上送本域信箱后从对端信箱拉取本 rank 所属切片归约累加） | [comm-engine-protocol-fault](symptoms/comm-engine-protocol-fault.md)（消息语义坏配方·归约读切片归属错——拉取源切片映射与切片归属映射两侧异表、拉取切片索引误按对端视角取〔应为**本 rank 归属视角**〕，host/kernel 载体均可；与收发分段倒置〔错值为**合法原值**〕按错值类别区分） |
| `HcclResult` / HCCL_E_PARA(1) | [collective-param-mismatch](symptoms/collective-param-mismatch.md) / [rank-group-mismatch](symptoms/rank-group-mismatch.md) / [comm-operator-anomaly](symptoms/comm-operator-anomaly.md) |
| `aclnn.*fail` / `561000` / `161001` | [comm-operator-anomaly](symptoms/comm-operator-anomaly.md) |
| `group` / `rankSize` / rank-id 碰撞 / comm init fail | 通信域参数不一致 → [rank-group-mismatch](symptoms/rank-group-mismatch.md)；init 失败/超时根因为 device 使用错误（多 rank 同卡争抢/硬编码同卡）→ [communication-timeout](symptoms/communication-timeout.md)（**B1** 建链分支） |
| `RANK-COLLISION` / worldSize 不一致 | [rank-group-mismatch](symptoms/rank-group-mismatch.md) |
| `error cqe` / `ROCE CQE ERROR` (EI0013) | 链路异常 → [communication-timeout](symptoms/communication-timeout.md)（触发 hccn-tool） |
| `wait socket establish timeout` / EI0006 | [communication-timeout](symptoms/communication-timeout.md)（建链阶段） |
| 多根因 / 单一根因修复后异常仍存 | [composite](symptoms/composite.md) |

命中「依边界规则 **B1/B2** 判主型」或出现多候选歧义时，读取[边界判定规则](boundaries.md)，按其中的判定次序区分。

## 二、工具索引

| 工具 | 文档 | 用途 | 何时调用 |
|------|------|------|---------|
| plog | [tools/plog.md](tools/plog.md) | 运行时日志采集与关键词过滤 | **首选**，所有通信异常的第一证据源；重跑取 plog 定位 rank/等待点 |
| msprof | [tools/msprof.md](tools/msprof.md) | HCCL 通信 Profiling 耗时分解 | 需量化等待时长、区分长等待方与晚到对端、核查负载偏斜或带宽瓶颈时 |
| msnpureport | [tools/msnpureport.md](tools/msnpureport.md) | device 日志导出与 bbox 取证 | plog device 日志含 AICPU/ECC/TS_ERROR，需核实 device 侧硬件或运行异常线索时 |
| hccn_tool | [tools/hccn-tool.md](tools/hccn-tool.md) | RoCE/HCCS 链路与网络诊断 | 超时阈值接近上限/cross-node 场景，需验证底层链路 UP/丢包时 |
| hccl_test | [tools/hccl-test.md](tools/hccl-test.md) | HCCL 通信带宽/正确性基准测试 | 剥离业务，在指定配置、设备与窗口下验证纯通信链路基准行为时 |
| msaicerr | [tools/msaicerr.md](tools/msaicerr.md) | AI Core 错误采集与硬件自检 | plog 含 TS_ERROR/aicore error，排查偶发计算错误是否硬件 ECC 时 |

**device id 约定**（关键易错点）：hccn_tool/hccl_test 用物理 chip id（示例 4,6）；msnpureport/msprof/msaicerr 用用户可见逻辑 device id（示例 0,1）；plog `deviceLogicId` 为驱动逻辑 device id，跨工具使用时须按 `npu-smi info` 与现场 `--help` 显式映射，见 [applicability.md](applicability.md) 与 [rank-localization.md](method/rank-localization.md)。

## 三、方法索引

| 方法主题 | 文档 | 内容 |
|---------|------|------|
| 日志关键词→异常类型映射 | [method/log-keyword-patterns.md](method/log-keyword-patterns.md) | plog/应用日志关键词正则模板、HcclResult/EI 错误码速查、层级判别口诀 |
| Rank 定位 | [method/rank-localization.md](method/rank-localization.md) | pid/localRankId/comm config 定位异常 rank、chip↔逻辑 id 映射、建链根节点定位、心跳定位 |
| 诊断决策树 | [method/decision-trees.md](method/decision-trees.md) | 从症状到根因的文本决策树（7 类异常分支判定）、层级判别速查表、排除规则 |

## 四、修复索引

**落码前必读**：[修复落码与自验](fix/common-constraints.md)说明接口核查、业务语义、通信匹配和输出验证；再按当前证据读取对应修复章节。

| 异常类型 | 修复章节 | 主修复类型 |
|---------|---------|-----------|
| communication-timeout | [fix/INDEX.md#communication-timeout](fix/INDEX.md#communication-timeout) | sync-relation-fix / communication-param-fix |
| collective-param-mismatch | [fix/INDEX.md#collective-param-mismatch](fix/INDEX.md#collective-param-mismatch) | sync-relation-fix |
| rank-group-mismatch | [fix/INDEX.md#rank-group-mismatch](fix/INDEX.md#rank-group-mismatch) | communication-param-fix |
| single-rank-block | [fix/INDEX.md#single-rank-block](fix/INDEX.md#single-rank-block) | sync-relation-fix |
| stream-event-cycle | [fix/INDEX.md#stream-event-cycle](fix/INDEX.md#stream-event-cycle) | sync-relation-fix |
| sync-imbalance | [fix/INDEX.md#sync-imbalance](fix/INDEX.md#sync-imbalance) | sync-relation-fix |
| comm-operator-anomaly | [fix/INDEX.md#comm-operator-anomaly](fix/INDEX.md#comm-operator-anomaly) | code-modification |
| comm-engine-protocol-fault | [fix/INDEX.md#comm-engine-protocol-fault](fix/INDEX.md#comm-engine-protocol-fault) | sync-relation-fix / code-modification |
| composite | [fix/INDEX.md#composite](fix/INDEX.md#composite) | 各单一修复的并集 |

## 渐进式发现路径

```
现象/日志关键词
  → [症状分流表] 候选类型
    → [symptoms/<type>.md] 调用工具 + 决策逻辑
      → [tools/<tool>.md] 工具输出解读 + 诊断决策
        → [method/decision-trees.md] 排除近似类型，锁定根因
          → [method/rank-localization.md] 精确定位 rank/等待点
            → [fix/INDEX.md#<type>] 修复策略 + 验证方法
```

## 文档清单

- L0 入口与环境约定（3）：`INDEX.md`（本文件）、`applicability.md`、`knowledge-access.md`
- L1 边界层（1）：`boundaries.md`（B1/B2 共存边界判定全文，按需读取）
- L1 症状层（9）：`symptoms/{communication-timeout,collective-param-mismatch,rank-group-mismatch,single-rank-block,stream-event-cycle,sync-imbalance,comm-operator-anomaly,comm-engine-protocol-fault,composite}.md`
- L1 工具层（6）：`tools/{plog,msprof,msnpureport,hccn-tool,hccl-test,msaicerr}.md`
- L2 方法层（6）：`method/{log-keyword-patterns,rank-localization,decision-trees,protocol-data-flow,native-fused-operators,aiv-transfer-alignment}.md`
- L2 修复层（2）：`fix/{common-constraints,INDEX}.md`

## 数据正确性方法入口

通信完成但数据不符、部分元素缺失或后续轮次错误时，读[通信数据流与完整输出核查](method/protocol-data-flow.md)。仍按源码证据区分调用参数、用户协议与计算实现，不能仅凭现象预设根因。
