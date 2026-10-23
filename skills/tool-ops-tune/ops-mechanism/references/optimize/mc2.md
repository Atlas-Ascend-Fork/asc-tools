# 机制卡：MC² 通算融合优化（Local Matmul 首选 + 通信掩盖 + 膨胀控制）

> 对应判定规则：L3_MC2_BALANCED / COMPUTE_BOUND / STRONG_COMPUTE / COMM_BOUND /
> STRONG_COMM / L3_MTE2_POLLUTED / L3_RANK_IMBALANCE。
> 内容沉淀自 MC² 通算融合调优实践（指标定义 / Bound 判定 / Local Matmul /
> 流水配平 / 膨胀分析），本卡将其机制决策浓缩为可执行顺序。
> **DSL 适用域（P2-5/G19）**：本卡实施细节以 **Ascend C** 为准；Triton / CATLASS / TileLang 的等价原语与实施差异见 [index.md 跨 DSL 原语对照表](index.md#跨-dsl-原语对照六原语--dsl)——跨 DSL 实施时以该表为路由、对应 DSL 专项文档为深度参考，不按本卡字面照搬。

## 策略优先级（与 R 值无关的固定顺序）

```
① Local Matmul（首选，不依赖 Bound 判定）
   本 rank 数据无需通信 → 独立计算与通信重叠。任何 R 档位都先做这一步。
② 流水配平（在 ① 基础上叠加）
   由 R 值与 TilingData 驱动的系统搜索（见 mc2-balancing.md），禁止 1-2 个经验值交付。
③ matmul 计算效率（仅计算 Bound，优先级最低，正交可叠加）
   转 ops-family:references/matmul/guide.md（pingpong/streamk/fullload…）。
```

### ① 的模式选型（蒸馏自 MC² 通算融合实践沉淀）

| 执行顺序 | 推荐模式 | 机制 |
|---------|---------|------|
| 通信后计算（Pattern A，如 alltoall+matmul） | **前置独立** | LocalMatmul 先执行，与首次通信重叠隐藏通信延迟 |
| 通信后计算且通信量小 / Local 占比小 | 融合 | self rank 在通信块循环内一起算（独立计算收益不大） |
| 计算后通信（Pattern B，如 matmul+alltoall） | **后置独立** | 通信块只算远程 rank，Local 后置不冲突 |
| 计算后通信且通信量小 | 融合 | 同上 |

**Local matmul 连续性约束**：前置/后置模式下 Local 数据段的 M 维切分必须连续
（否则 WaitFlag 顺序与数据顺序错位）。

## 各 R 档位的机制方向

| 档位 | R | 机制方向 |
|------|---|---------|
| 强通信 | <0.5 | 从短块出发搜索，最大化 tileCnt（计算块尽量小、轮次尽量多） |
| 通信 | <1 | 用计算掩盖通信，增大 tileCnt |
| 平衡 | 0.9~1.1 | 等分切分即最优，配平空间小 |
| 计算 | 1.1~2 | 用通信掩盖计算，增大 tileCnt |
| 强计算 | >2 | 增大 tileCnt 收益极小，倾向**少切分**（tileCnt 小、tile 大）+ 叠加 ③ |

## MTE2 污染（L3_MTE2_POLLUTED 命中）的路由

CrossCoreWaitFlag 挂在 AIC MTE2 流水线等待 AIV 通信 → aic_mte2_ratio 含通信等待空转：

- **单核访存机制全部无效**（判定引擎已禁用 L1_MTE2_BOUND，机制路由改道本卡）；
- 有效方向 = 消除/减少通信等待本身：Local Matmul、配平（减少互等）、Pattern A/B 的
  同步粒度（CrossCore flag 粒度放宽 —— 实施细节需自行论证并提高验证强度）。

## 膨胀分析（切分参数变更时必做）

切分变化同时引入两类膨胀，**最小化瓶颈方膨胀**（被掩盖方膨胀影响小）：

| 膨胀类型 | 表现 | 判读 |
|---------|------|------|
| 计算膨胀 | 流水断流 / 用核不满 / mac_ratio 下降 | 每块 matmul 效率损失（尾块/小 tile） |
| 通信膨胀 | 头开销分摊变差 / 带宽下降 | 分片数增加 → 单片通信效率下降 |

配平搜索的剪枝规则（四条，见 mc2-balancing.md）已内置膨胀可行性判断；Top-N 候选
必须**实测**（隔离测试已产出 T_comm/T_compute，核算成本极低）。

## 性能指标核算（每方案必选输出）

```
加速比 = 融合前耗时（tileCnt=1）/ 融合后耗时
掩盖率 = (融合前耗时 − 融合后耗时) / min(T_compute, T_comm)     # 上限 1.0
跨 rank：T_task = max(rank 稳态均值)                              # 木桶效应
```

## 前置数据硬约束

| # | 约束 |
|---|------|
| MC-1 | 缺 TilingData（baseM/baseN/baseK/usedCoreNum，含长短块）不得做配平分析；host 未打印则在源码副本启用 PrintTilingData 补采 |
| MC-2 | 配平方案必须系统搜索（四剪枝 + Top-N 实测），禁止经验值直接交付 |
| MC-3 | 隔离测试在源码副本进行，产物保留、代码副本丢弃 |
| MC-4 | 一切性能数据出自 msprof Task Duration（task-based），禁 host 计时 |

## 验证与回滚

- 精度：全量 case 工程容差（通信+计算双路径都要过）；
- 性能：跨 rank max 口径对比（delta_compare 会拒绝跨 rank 口径不一致的对比）；
- 回滚：配平搜索候选逐个实测，劣化候选直接弃用（搜索本身就是多候选实测，负结果
  记入报告「保留与回滚清单」——它是瓶颈证据的组成部分）。

## 精度风险

Local Matmul 是数学等价改造（语义保持）；配平改变归约/累加分块顺序时属语义变更
（浮点结合律），须边界值/大 shape 验证。

## 性能抖动诊断（L3_RANK_IMBALANCE 命中的下钻，蒸馏）

op_summary 的 Device_id 列识别多卡参与：同卡多行 = 多轮执行，不同卡同序号行 =
同轮次。**时间维度**（同卡跨轮次）与**空间维度**（同轮次跨卡）分别分析
aiaiv/mte 指标的离散度：
- 时间抖动大（同卡 p95≫p50）→ 系统噪声（DVFS/同卡负载），加轮次取稳态而非
  追机制；
- 空间抖动大（同轮跨卡差异）→ 真 rank 不均衡，走配平/Local Matmul 机制。
（snapshot 的 p50/p95 分布字段 + delta_compare 抖动诊断段已承载该数据。）

## 模板资产

`ops-family:references/mc2/guide.md`（3 策略 + 模板代码）。
