# 输入数据源契约（npu-compute .npu-rep 与 msopprof PROF_* 两源）

> tool-perf-analysis 的输入边界：只读用户给定的采集产物 + 本 SKILL 自带资源，
> 不启动采集（采集属 tool-ops-tune ops-collect / 用户的采集流程）。

## 源 A：npu-compute `.npu-rep`（Ascend 950 / dav-3510 专用）

950 上用 npu-compute 采集（`npu-compute [--set basic|full] <program>`），产物为
`.npu-rep` 二进制容器（NpuRepHead + 文件表；magic `npu-rep\0`，嵌套 type=1 子报告）。
本 SKILL 用 `scripts/unpack_npu_rep.py` 解包（也可用 `npu-compute --import`）。

| 解包产物 | 内容 | 消费方 |
|---------|------|-------|
| `summary.jsonl` | 末行 OpInfoSummary（算子名/类型/Task Duration/Block Dim/频率/理论算力/GM 带宽利用率）+ 前部 Section Task 级汇总行 | 基本信息 + L2 标量 |
| `HardwareInfo.jsonl` | 芯片型号 / 架构（3510→a5）/ AIC·AIV 核数 / 频率 / HBM | 芯片识别 + soc-spec 解析 |
| `PipeUtilization.csv` | 逐 block pipe 占比（aic_/aiv_ 双列）+ active_bw + icache_miss | verdict.py --csv（L1/L2 判定主输入） |
| `ArithmeticUtilization.csv` | vec/vf/sfu/simt_vf 占比 | vec_fp32_ratio 反证标量 |
| `ResourceConflictRatio.csv` | LDU/STU/SFU 冲突 + wait_ratio | vec_total_cflt_ratio 反证标量 |
| `L2Cache.csv` | 近端/远端 hit/miss/victim + 读写命中率 | l2_hit_rate（L0 规则） |
| `Memory.csv` / `MemoryUB.csv` / `MemoryL0.csv` | 通路带宽与数据量 | 冗余搬运/带宽口径注记 |
| `PipeTrace.json` | Chrome Trace 流水忙碌区间（**最多采样 6 个 Group**，不代表全部核） | trace 汇总图 |
| `summary.db`（PcSampling 表） | PC 采样 stall 枚举（scoreboard_not_ready 等 7 项） | top_stall_pct（L0_STALL_DOMINANT） |
| `objdump.txt` / `symbolizer.json` | 反汇编 + 源码行映射 | stall 热点定位到代码行（报告附注） |

注意：仅 `Pipeline` Section 时无 CSV（如 verify_pipeline_pm.npu-rep）——此时
L1/L2 规则按绑定契约 SKIPPED，判定由 L0（PcSampling）+ summary 标量承担，报告
数据缺口列出「补采 --set full 可获得逐 block 判定」。

## 源 B：msopprof `PROF_*` 目录（非 950 芯片：A2/A3 等）

非 950 用 msopprof 采集（`msopprof --application=<exe> --aic-metrics=PipeUtilization,... --output=<dir>`），
产物目录含：

| 文件 | 内容 | 消费方 |
|------|------|-------|
| `OpBasicInfo.csv` | Op Name / Op Type / Task Duration(us) / Block Dim / 频率 | OpInfoSummary 等价物 + L2 标量 |
| `PipeUtilization.csv` | 逐 block pipe 占比（列名与 npu-compute 同族；A2/A3 含更细 scalar stall 分解列） | verdict.py --csv |
| 其余 `<Metric>.csv` | 按采集时 --aic-metrics 选择 | 同源 A（缺项如实标注） |
| `dump/`、`visualize_data.bin` | 原始二进制（不解析） | — |

差异点（与源 A）：
- 无 HardwareInfo → **必须 `--soc a2|a3|a5` 显式指定**（soc-spec 峰值换算依赖）；
- 无 PcSampling / PipeTrace / objdump → L0 stall 档与 trace 汇总图跳过（如实标注）；
- msopprof 的 aiv_scalar_ratio 在 SIMT 内核上可与 vec/mte 并行超 100%（pipe 间
  并行重叠），pipe_sum>115% 是「多 pipe 并行」证据而非数据可疑——与 npu-compute
  串行求和口径区分解读。

## 口径规则（两源通用）

- `NA` = 不适用/未采集，剔除不按 0 统计；`aic_*` 整列 NA → 未使用 Cube 核；
- ratio 类 0~1 / 0~100 双口径归一（verdict.py 同款边界 ≤1.5）；
- 活跃带宽（active_bw）是**搬运活跃期间**带宽，全程平均 ≈ active_bw × ratio；
  GM 带宽利用率优先用 OpInfoSummary `aicore_gm_bw_usage_rate(%)`（Σ块口径）；
- 单次采集（single-shot）：结论标注非稳态，稳态须 warmup≥10 / repeat≥5 / median；
- 降频（Current < Rated × 0.9）标注「降频失真」；
- 多 kernel（Op Name 多值）必须 --kernel-name 过滤，否则不产出跨 kernel 平均。
