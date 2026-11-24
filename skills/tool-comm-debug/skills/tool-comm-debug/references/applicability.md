# 适用范围与限制

使用本 Skill 前先确认本页条件。任一条件不满足时，说明不支持的具体条件，不强行进入诊断流程。

## 适用范围声明

- soc：Ascend 系列 NPU 单机多卡（**触发不限代际**——本 Skill 的触发面按"Ascend 单机多卡通信异常"现象匹配，不限定具体芯片代际；验证基线与跨代际使用纪律见下「验证环境记录与移植指导」）。
- cann：CANN 9.0.0 是历史验证基线，不是仅允许该版本的门禁。实际使用核对构建与运行加载的版本/路径、所需公开API与引擎是否可用；版本不同本身不构成拒绝理由。历史工具链依赖下界 ≥8.5（runtime / ge-executor / metadef / asc-devkit / bisheng-compiler），具体工程要求以其声明与实际构建为准。
- bisheng：bisheng-compiler ≥8.5。
- op_class：HCCL 集合通信与点对点通信编排（AllReduce / Broadcast / Reduce / AllGather / ReduceScatter / Send / Recv / BatchSendRecv 等）及自研主机侧/设备 kernel 通信引擎的用户协议层。
- 诊断工具使用 CANN 标准安装路径下的 plog / msprof / msnpureport / hccn_tool / hccl_test / msaicerr。
- device id 约定（关键易错点）：区分三类编号域——① 用户可见逻辑 device id（受 `ASCEND_RT_VISIBLE_DEVICES` 裁剪后的 `0..N-1`，供 `aclrtSetDevice` / `aclrtGetDevice` 及 msprof / msnpureport / msaicerr 使用）；② 物理 chip / NPU id（供 `hccn_tool` / `hccl_test` 使用）；③ 驱动逻辑 device id（部分底层物理内存映射与访问权限接口如 `aclrtMemSetAccess` 按驱动域编号寻址；单进程仅暴露非 0 物理卡时，用户逻辑号 `0` 与驱动逻辑号不同）。接口报参数无效（如 `507899`）时，逐阶段核对物理分配、虚拟地址映射、访问权限设置到通信初始化各调用所需的编号域契约。对照见 [INDEX.md](INDEX.md) 工具索引与 [rank-localization.md](method/rank-localization.md)。

## 验证环境记录与移植指导

适用性分三层，使用前按层核对：

1. **触发层（不限代际）**：触发面按"Ascend 系列 NPU 单机多卡通信异常（HCCL 集合通信/MC2 通算融合调用域）"现象匹配，非 910B1 独占——其他 Ascend 代际的同类现象现场同样进入本 Skill 流程。
2. **验证基线层（verified_on）**：soc=Ascend 910B1；cann=9.0.0。本 Skill 的症状分流表、共存边界判定规则（B1/B2）、rank 定位方法与修复模式，在 112 例真实 910B1 单机多卡异常现场（9 类异常，含修复后真机数据级校验回归）上采集、验证并多轮迭代。该基线是**已验证事实的覆盖范围声明**，不反推其他代际不可用。
3. **移植指导层（其他 Ascend 代际，如 Ascend 950 / Atlas A3 系列）**：诊断方法论（症状分流 / 异常 rank 定位 / 决策树）与六类诊断工具用法可按类别迁移，但旧基线不能证明当前环境已验证。工具输出格式、日志路径、错误码和选项可能随版本漂移；按[知识访问说明](knowledge-access.md)检索固定版本知识卡、核对其官方来源及现场工具 `--help`，记录实际版本与输出。知识未覆盖或证据不足时标记未知，不通过推测补造结论。

## 不适用边界

- 纯硬件故障排查（掉卡、器件损坏、供电/温度告警）：按硬件运维流程处理。本 Skill 仅在通信证据指向硬件疑似（ECC / SVID / HWERR 等）时，给出转硬件排查的建议路径，不替代硬件诊断。
- 单卡、非通信类问题：算子数值精度、性能调优不在范围内。
- 非 Ascend 平台（如 CUDA/NCCL 体系）：工具链与判定规则均不适用。

## 结论边界

- 诊断结论只覆盖本次现场证据（日志 / plog / 退出码 / 源码）能支撑的范围；判定要件不满足时输出「证据不足 + 待采集清单」，不推测。
- 仅基于本次现场证据给出结论；未实际复验时，不声称已完成验证。
- 静默无报错的现场（stderr 为空、无超时/原语报错行）不等于无异常——编排层超时兜底、数据级校验失败等非显式信号同样构成证据，见 [INDEX.md](INDEX.md) 症状分流表相应条目。
