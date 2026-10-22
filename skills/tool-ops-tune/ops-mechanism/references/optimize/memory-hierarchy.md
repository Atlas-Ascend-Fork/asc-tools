# 机制卡：存储层次优化（L2/UB/L1/L0 复用）

> 对应判定规则：L1_MTE2_BOUND（真饱和档）、L0_L2_LOW_HIT、L1_VEC_BOUND（UB 往返类）。
> 内容沉淀自存储层次调优一线实践（L2 命中率判读、UB 常驻复用设计）。
> **DSL 适用域（P2-5/G19）**：本卡实施细节以 **Ascend C** 为准；Triton / CATLASS / TileLang 的等价原语与实施差异见 [index.md 跨 DSL 原语对照表](index.md#跨-dsl-原语对照六原语--dsl)——跨 DSL 实施时以该表为路由、对应 DSL 专项文档为深度参考，不按本卡字面照搬。

## 机制清单（按层次）

| 层次 | 机制 | 适用证据 | 关键约束 |
|------|------|---------|---------|
| L2 | `SetL2CacheHint(CacheMode::CACHE_MODE_NORMAL)` | L2 hit <50% 且数据有复用 | — |
| L2 | 只读一次数据 `CACHE_MODE_DISABLE` | 流式数据污染 L2 | 与上一条二分：有复用开 NORMAL，无复用 DISABLE |
| L2 | 工作集切分（访问序改造） | 大工作集整体 miss | L2 参数随 SoC 变化，须重算容量 |
| L1 | B 矩阵/小侧矩阵驻留（fullload） | 小侧 ≤L1/2 + 对侧 T≥2 + 真 MTE2 bound | L1 预算 = 分形 + ping-pong 缓冲 |
| L0 | L0C 累加（多 matmul 累加不回写） | CUBE bound + Fixpipe 高 | 累加链精度（语义变更类） |
| UB | tile/loop 间常驻复用（TBuf 替代重复 MTE2） | 多 tile 重复搬同一数据 | UB 容量 + Bank 冲突规避（padding 32B 整数倍） |
| 寄存器 | 中间值留寄存器/areg | 短生命周期中间量 UB 往返 | 寄存器压力与 spill 反证 |

## Bank 冲突规避（常驻复用的伴生问题）

| 冲突类型 | 判定字段 | 修法 |
|---------|---------|------|
| bankgroup | vec_bankgroup_cflt_ratio >3% | 修改 repeatStride/blockStride |
| bank | vec_bank_cflt_ratio >3% | UB tensor 起始地址 + 32B 整数倍 padding |
| 资源 | vec_resc_cflt_ratio >5% | 错开 Vector/Cube 调度 |
| MTE | vec_mte_cflt_ratio >3% | 搬运与计算时序调整 |

## 验证

L2Cache.csv 复采（hit rate 变化）+ verdict 复跑；驻留类改造须核对 L1/L0/UB 三级预算
表（改一处动全身——跨层耦合）。

## 回滚

hit rate 未升反降 / UB 容量挤压导致 tile 变小引起 MTE 次数上升 / 精度失败 → revert。

## 模板资产

`ops-family:references/common/ub_resident_design.md`；
matmul fullload：`ops-family:references/matmul/guide.md`。
