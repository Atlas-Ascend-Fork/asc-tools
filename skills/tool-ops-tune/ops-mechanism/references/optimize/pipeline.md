# 机制卡：流水优化（Double Buffer / 流水掩盖 / 同步降本）

> 对应判定规则：L1_SERIAL_NO_OVERLAP、L1_LATENCY_SYNC_SUSPECT、L1_FIXP_BOUND（气泡相关）、
> L0_STALL_DOMINANT。跨 DSL 原语对照见文末。
>
> 内容沉淀自 Ascend C 双缓冲与流水判读的一线实践，跨 DSL 原语对照见文末。

## 原理

单缓冲下 MTE 搬运与计算串行：pipe busy 求和 ≈100% 但各 pipe 均不满 → 「串行无重叠指纹」
（判定规则 L1_SERIAL_NO_OVERLAP 命中即此形态）。双缓冲把 tile 搬运与上一 tile 计算
重叠：理想收益上界 ≈ max(t_copy, t_compute) / (t_copy + t_compute)。

## 适用前提

- verdict 命中 L1_SERIAL_NO_OVERLAP 或 LATENCY/SYNC 可疑 + 反证「串行指纹」；
- UB/L1 容量预算：双缓冲 = 2×tile 缓存，须核对 TilingData 的 buffer 规划；
- 循环内无跨 tile 数据依赖（有依赖需先做依赖分析或改滚动窗口）。

## 实施形态（分场景）

| 场景 | 形态 | 关键点 |
|------|------|--------|
| TQue 标准流水 | `InitBuffer(queue, 2, size)` | bufNum=2；EnQue/DeQue 配对；循环内无多余 Sync |
| 非 TQue（裸 MTE + 事件） | ping-pong 两组 UB + event id | 须自行论证 prologue/drain 段（已知知识缺口，验证强度↑） |
| Cube-Vector 混合 | workspace 份数 2→4 | 减少 Cube/Vector 互等（GroupedMatmul 案例 154.2→131.8us） |
| AIC:AIV 配比 | 1:1 → 1:2 | Cube/Vector 耗时严重不匹配时（同案例 218.1→154.2us，收益最大） |
| 同步删减 | 删除经依赖证明冗余的 SetFlag/WaitFlag/barrier | 每删一处必须给出依赖论证（精度/正确性风险点） |

## 验证（每轮门禁）

```bash
# ① 精度全量（工程现有测试，禁止自创容差）
bash run.sh --verify-all
# ② 同口径性能（与基线同 timing_method/warmup/repeat）
bash run.sh --perf  # event 口径
# ③ 机制生效证据：DoubleBuffer 生效 = MTE 与计算重叠比例 >30%（部分 10-30%，未生效 <5%）
python3 ../ops-verdict/scripts/verdict.py --rules ../ops-verdict/rules/L1-single-core.yaml \
    --csv <new PipeUtilization.csv> --soc <soc> -o round-N.verdict.json
# 期望：__pipe_sum_mean 明显下降或 L1_SERIAL_NO_OVERLAP 转为 NOT_HIT
```

**重叠比例的直接度量（P1-1，G8/G13 验证闭环）**：上板有 A5 PipeTimeline 或
仿真路径（cannsim）时，用 trace 工具替代间接指纹：

```bash
# 仿真路径：collect_sim.sh 采 trace → 重叠度标量
bash ../ops-collect/scripts/collect_sim.sh --exe <exe> --soc-version Ascend950 --output ./prof/sim
python3 ../ops-verdict/scripts/trace_bubble.py --trace-dir ./prof/sim/cannsim_*/report/ \
    --scalar-out overlap_scalars.json
# overlap_pct 即「MTE×计算 busy 交集/并集」的实测值（本卡阈值的直接对照）：
#   >30% 生效 ｜ 10-30% 部分生效 ｜ <5% 未生效
# KEEP 结论附 overlap_pct 前后对照（机制生效判读，而非仅 speedup）
```

上板无 trace 设施时，`__pipe_sum_mean` 间接指纹照旧（medium 置信度如实保留），
报告标注「重叠度为间接推断」。

## 无 Bound 形态（NO_BOUND_FOUND / 全 pipe 低占用，沉淀自一线调优实践）

各单元利用率均不高、无明显单一瓶颈时，流水化优先于单点优化：

| # | 机制 | 说明 |
|---|------|------|
| 1 | PingPong 粒度调优 | 控制每次搬入量，确保**计算时间 ≥ 搬运时间**（否则 DB 反而暴露搬运瓶颈） |
| 2 | MMAD→Fixpipe UnitFlag 同步 | 用 UnitFlag 信号替代显式 Barrier（MMAD 输出→Fixp 输入硬依赖场景） |
| 3 | 减少单次搬运量（渐进式） | 边算边搬不积压——避免「全量搬完才开始算」 |
| 4 | Preload 窗口 | 当前计算未完成时启动下一批搬入；窗口大小匹配计算耗时（搬完算没完 = 白搬） |
| 5 | 指令提早发射 | 搬移指令不等当前计算完全结束；计算流水线填充 |
| 6 | Vec 融合减重复搬运 | 同一数据多次搬入场景：寄存器内连续处理 + 驻留期间全量消费 |

trace 判读（trace_bubble）：MTE2 全在前 VECTOR 全在后 = DB 未生效；交替出现
= 流水健康。tiling 修正：tile 粒度匹配 PingPong 分段；DB 使能时实际可用 UB ÷2。

## 回滚条件

- speedup < 1.05（噪声带内）→ 加采样轮次复测，仍带内 → revert；
- 精度任一 case fail → revert（该轮性能数字不进报告）；
- 容量联动被破坏（tile 改动后 device fault）→ revert + 缩小改动粒度（如 baseK 128→192
  而非直接 256）+ msSanitizer 定位。

## 精度风险分级

- 语义保持类：双缓冲、搬运合并、调度顺序、同步删减（须依赖论证）→ 常规门禁。
- 语义变更类：归约顺序改变（浮点结合律）→ 额外边界值/大 shape 验证用例。

## 跨 DSL 原语对照（双缓冲）

| DSL | 双缓冲实现 |
|-----|-----------|
| Ascend C | `InitBuffer(queue, 2, size)`（TQue）/ ping-pong 两组 UB + event id（非 TQue） |
| CATLASS | `MmadAtlasA2Pingpong` STAGES 参数 |
| Triton-Ascend | UB 留 50% 余量 + num_stages |
| TileLang | DB-ANALYSIS 分析后实施 |
| SHMEM/MC² | 通信-计算 ping-pong（CrossCore flag 同步，见 mc2.md） |
