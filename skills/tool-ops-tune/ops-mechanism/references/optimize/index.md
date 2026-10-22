# 机制索引（optimize-index）

> 消费时机：verdict 判定完成后按 findings[].mechanism_refs 路由，只加载命中的单张卡。
> **证据先行：没有 verdict 不加载机制卡**（禁止先看机制猜瓶颈）。

## 索引表（rule_id → 机制卡）

| rule_id | 机制卡 | 一句话机制 |
|---------|--------|-----------|
| L1_MTE2_BOUND | [data-copy.md](data-copy.md) | 真饱和→数据量/复用；假 bound→对齐/合并小搬运 |
| L1_MTE3_BOUND | [data-copy.md](data-copy.md) | 搬出侧同二分 |
| L1_FIXP_BOUND | [data-copy.md](data-copy.md) | 512B 地址对齐 |
| L1_VEC_BOUND | [api-usage.md](api-usage.md) + [ascendc-regbase-vf.md](ascendc-regbase-vf.md)（A5 深度档） | UB 融合/减 Cast/融合指令；VF 范式 |
| L1_CUBE_BOUND | [ascendc-tiling.md](ascendc-tiling.md) + ops-family matmul | tiling / L1 复用 / L0C 累加 |
| L1_SCALAR_BOUND | [api-usage.md](api-usage.md) | 标量削减六法 |
| L1_LATENCY_SYNC_SUSPECT | [pipeline.md](pipeline.md) | Double Buffer / 同步删减 |
| L1_SERIAL_NO_OVERLAP | [pipeline.md](pipeline.md) | 双缓冲全形态 + workspace 份数 + AIC:AIV 配比 |
| L2_CORE_TAIL / L2_IMBALANCE_* | [ascendc-tiling.md](ascendc-tiling.md) | 尾块均衡 / swat / NUMA 偏斜处置 |
| L2_UNDERUTILIZATION | [ascendc-tiling.md](ascendc-tiling.md) | Block Dim 提升（头开销权衡） |
| L2_HEAD_OVERHEAD | [api-usage.md](api-usage.md) | 减核 / 缩 TilingData / TPipe 外置 |
| L3_MC2_*（全档） | mc2.md（随多卡域 PR 交付） | Local Matmul 首选 + 通信掩盖 + 膨胀控制 |
| L3 配平环节 | mc2-balancing.md（随多卡域 PR 交付） | TilingData 驱动系统搜索（四剪枝 + Top-N） |
| L3_MTE2_POLLUTED | mc2.md（随多卡域 PR 交付） | 单核访存机制无效，改道通信掩盖 |
| L3_RANK_IMBALANCE | mc2.md（随多卡域 PR 交付） | 跨 rank 长尾排查 |
| L0_ICACHE_MISS | [api-usage.md](api-usage.md) | 代码布局/分支整理 |
| L0_L2_LOW_HIT | [memory-hierarchy.md](memory-hierarchy.md) | CacheMode 二分 + 工作集切分 |
| L0_STALL_DOMINANT | [pipeline.md](pipeline.md) | stall 枚举 → 对应流水机制（M2 细化） |

## 跨 DSL 原语对照（六原语 × DSL）

| 原语 | Ascend C | CATLASS | Triton | TileLang | SHMEM/MC² |
|------|---------|---------|--------|----------|-----------|
| 双缓冲 | InitBuffer(q,2,sz) / ping-pong+event | STAGES 参数 | num_stages + UB 余量 | DB-ANALYSIS | 通算 ping-pong（CrossCore） |
| 流水掩盖 | 事件同步 MTE/计算 | pipeline policy | 软件流水 | schedule 原语 | 通信-计算交错 |
| 同步降本 | 删冗余 SetFlag/WaitFlag | — | barrier 合并 | — | flag 粒度放宽 |
| 分核均衡 | blockDim/尾块/swat | BlockScheduler-Swizzle | 网格切分 | tile 划分 | rank 分片 |
| 存储层级 | UB/L1/L0 驻留 | L1/L0TileShape | — | — | 远端 window 驻留 |
| 标量削减 | Muls/常量折叠/外提 | — | constexpr 化 | — | — |

## 已知覆盖缺口（降级策略见 SKILL.md）

- FA 类混合绑核（AIC+AIV CrossCore mode2）：跨 j 迭代 GM buffer ping-pong、flag 粒度
  放宽、GM→L1 滚动预取只有通用原理，实施细节自行论证 + 提高验证强度；
- MoE routing / key-value sort / gather-scatter / histogram（SIMT 不规则读写）；
- Double Buffer 非 TQue 形态（ping-pong event id + prologue/drain 自行论证）。
