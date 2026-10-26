# 机制卡路由表（verdict rule_id → tool-ops-tune ops-mechanism 卡要点）

> 本文件是 tool-perf-analysis 对 tool-ops-tune 机制卡的**本地路由摘要**，仅用于把
> verdict.json 的 `findings[].mechanism_refs` 翻译为报告中的候选方案卡要点。
> 机制卡正文（原理/适用前提/预期收益/架构限制/精度风险/验证命令/回滚条件八要素）
> 的唯一真源是 tool-ops-tune `ops-mechanism/references/optimize/*.md`——
> 本 SKILL 运行时优先直接读取并内嵌机制卡正文（diagnose.py 已实现）；
> 仅当 tool-ops-tune 不可寻址（离线降级）时才用本摘要兜底，且报告须标注
> 「降级摘要，机制细节以 tool-ops-tune 机制卡为准」。

## 路由表

| rule_id | 机制方向（一句话） | 机制卡 | 收益区间（speedup_source 口径） |
|---------|------------------|--------|------------------------------|
| L1_MTE2_BOUND | 真带宽饱和→数据量/复用；假 bound（busy 高带宽低）→对齐/合并小搬运 | data-copy.md | [1.2, 2.0] 🟡 首期标定 |
| L1_MTE3_BOUND | 搬出侧同真假二分 | data-copy.md | [1.1, 1.4] 🟡 |
| L1_FIXP_BOUND | 512B GM 地址对齐（fixpipe>15% 是直接信号） | data-copy.md | [1.1, 1.3] 🟡 |
| L1_VEC_BOUND | UB 融合/减 Cast/融合指令；A5 深度档走 RegBase/VF 范式 | api-usage.md + ascendc-regbase-vf.md | [1.1, 1.5] 🟡 |
| L1_CUBE_BOUND | tiling / L1 复用 / L0C 累加（matmul 族叠加 ops-family） | ascendc-tiling.md + ops-family matmul | [1.1, 1.5] 🟡 |
| L1_SCALAR_BOUND | 标量削减六法（缩 TilingData/减核/TPipe 外置/外提/向量化/地址优化） | api-usage.md | [1.1, 1.5] 🟡 |
| L1_LATENCY_SYNC_SUSPECT | Double Buffer / 同步删减（全 pipe 低占用≠无 bound） | pipeline.md | [1.2, 2.5] 🟡 |
| L1_SERIAL_NO_OVERLAP | 双缓冲全形态 + workspace 份数 + AIC:AIV 配比 | pipeline.md | [1.3, 2.0] 🟡 |
| L2_CORE_TAIL / L2_IMBALANCE_* | 尾块均衡 / block swat / NUMA 偏斜处置 | ascendc-tiling.md | [1.05, 1.3]（长尾）/ [1.1, 1.5]（严重档）🟡 |
| L2_UNDERUTILIZATION | Block Dim 提升（最低垂果实；Task<25us 须权衡头开销） | ascendc-tiling.md | [1.2, 3.0] 🟡 |
| L2_HEAD_OVERHEAD | 减核 / 缩 TilingData / TPipe 外置 | api-usage.md | [1.05, 1.2] 🟡 |
| L3_MC2_*（全档） | Local Matmul 首选 + 通信掩盖 + 膨胀控制（不依赖 bound 判定先做 ①） | mc2.md | 按档位 [1.0, 2.0] |
| L3 配平环节 | TilingData 驱动系统搜索（四剪枝 + Top-N，mc2_search.py） | mc2-balancing.md | — |
| L3_MTE2_POLLUTED | 单核访存机制无效，改道通信掩盖（L1_MTE2_BOUND 已被路由禁用） | mc2.md | — |
| L3_RANK_IMBALANCE | 跨 rank 木桶长尾排查 | mc2.md | [1.0, 1.15] 🟡 |
| L0_ICACHE_MISS | 代码布局/分支整理 | api-usage.md | [1.0, 1.1] 🟡 |
| L0_L2_LOW_HIT | CacheMode 二分（有复用 NORMAL / 流式 DISABLE）+ 工作集切分 | memory-hierarchy.md | [1.1, 1.4] 🟡 |
| L0_STALL_DOMINANT | stall 枚举 → 对应流水机制（scoreboard_not_ready=访存等待 → data-copy/pipeline） | pipeline.md | [1.0, 1.2] 🟡 |

## PcSampling stall 枚举 → 机制方向（npu-compute .npu-rep 独有）

| stall 项 | 含义 | 机制方向 |
|----------|------|---------|
| scoreboard_not_ready | 访存依赖等待（gather/scatter/随机访问的 store/load 未就绪） | data-copy.md（向量化 gather、布局重构）+ pipeline.md（访存掩盖） |
| nop_cycles | 空转周期占比 | pipeline.md（流水填充/空泡消除） |
| register_bank_conflict | 寄存器 bank 冲突（SIMT） | api-usage.md / ascendc-regbase-vf.md |
| resource_conflict | 资源冲突 | api-usage.md |
| warp_level_sync | warp 级同步等待 | pipeline.md（同步删减） |
| divergence_stack_spill | 分支发散栈溢出（SIMT） | api-usage.md（分支整理/布局） |
| ibuf_empty | 取指饥饿 | api-usage.md（循环体过大） |

## 收益引用纪律（与 tool-ops-tune 一致）

- 区间均标注 🟡 首期标定 / [tuning-practice] 等溯源口径，属**典型收益/实测参考**，
  不得表述为对本算子的预测；
- 本算子实际增益须按机制卡「验证」小节同口径重采实测；
- 单变量铁律：一次只实施一个机制，diff_audit 校验改动范围后 delta_compare 出
  KEEP/REVERT/噪声带三段结论。
