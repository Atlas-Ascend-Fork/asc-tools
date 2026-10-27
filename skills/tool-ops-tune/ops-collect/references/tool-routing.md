# 采集工具路由决策树（跨版本能力矩阵）

> 硬规则：**先探测本机帮助，再选命令**。本矩阵是选型起点而非命令来源——所有参数、
> 指标名、能力开关以本机 `msprof op --help` / `msopprof --help` / `npu-compute
> --list-sets` 实际输出为门禁。禁止使用「MC² 永远不能用 msprof op」这类跨版本
> 绝对规则（新版已支持时可用，旧版或不稳定时回退完整 msprof）。

## 决策树

```
① 判算子形态
   MC²/fork 多 rank ──────────────→ msprof --aic-mode=task-based（本机 msOpProf 明确支持
                                    多设备/通算流水时也可用 msprof op，否则回退）
   单卡算子 ──→ ②
② 判芯片与采集格式
   950/A5 且本机 npu-compute 可用 → npu-compute（DB 格式采集，Roadmap #4：
                                    --set full + --sections 追加按 --list-sections 门禁；
                                    编排：ops-collect/scripts/collect_board.sh --mode npu-compute
                                    → .npu-rep → --import 解包 → summary.db（含 PcSampling）
                                    → facts_bridge DB 通道 / verdict --db）
   其他 / npu-compute 不可用 ──→ ③
③ 判分析深度
   只要 kernel 时间/加速比 ────────→ quick 路径（1 轮 Task Duration）
   要瓶颈判定 ──→ ④
④ 判芯片与本机能力（--help 探测）
   A2/A3：MemoryDetail / TimelineDetail 可用 → 按疑点选配
   A5：PipeTimeline / PcSampling 可用 → 优先评估（pipe 重叠 / 指令热点）
   通用底座：7 组 aic-metrics 顺序采集（PipeUtilization 为判定引擎主输入）
⑤ 无卡 / 指令级下钻 ──────────────→ cannsim（仅 A5 单卡 AI Core；MC²/HCCL 不支持）
                                    编排：ops-collect/scripts/collect_sim.sh（P1-1）
                                    record → report(trace_core*.json) → mode=sim manifest
                                    → trace_bubble.py（overlap/top_stall/bubble 标量）
⑥ torch 生态 ────────────────────→ torch_npu.profiler（JSONL 用例，warmup/active 固定 5）
                                      JSONL → cases.csv 入口：ops-collect/scripts/jsonl_to_cases.py
                                      （P1-5；schedule 5/5 由 torch 侧基准脚本持有，
                                       产物兼容性评估后接入 verdict 链路——当前标注
                                       🟡 评估中，未接入主判定链）
⑦ 集群视角（scene=cluster，P2-4 规划态）→ msprof advisor（集群/通信分析路径；
                                      本机 --help 为门禁同款纪律）。范围边界：本仓
                                      只消费「判定引擎可读的集群级证据」（快慢 rank /
                                      host 下发阻塞），深度集群分析是 Hermes 主场；
                                      算子域聚焦不变
```

## 工具能力矩阵（以本机帮助为最终门禁）

| 能力 | msprof | msprof op / msopprof | npu-compute | cannsim | torch_npu.profiler |
|------|--------|---------------------|---------|---------|--------------------|
| MC² 多 rank | ✅ task-based | 🟡 依版本（探测后用） | ❌（单进程直调形态） | ❌ | ❌ |
| 7 组 aic-metrics | ✅ | ✅（多轮 replay 内置） | ✅（--set full 一次采集，无需多轮 replay） | 部分（PipeUtilization+ResourceConflictRatio 默认） | 汇总级 |
| Roofline | ✅（组合 Default） | 🟡 依版本 | —（OpInfoSummary 带理论算力/带宽与实测利用率读数） | — | — |
| KernelScale / Occupancy | ✅ | ✅ | ✅（逐核） | ✅（逐核） | — |
| 指令级 / 源码行 | — | Source（-g） | —（PmSampling 指令地址采样 + objdump/symbolizer 定位） | ✅（code_exe/instr_exe） | — |
| A5 PipeTimeline / PcSampling | — | ✅（A5） | ✅（PipeTrace.json + summary.db PcSampling 表） | — | — |
| **DB 格式产物** | msprof_*.db（task_time，P1-6 双源） | — | ✅ **summary.db**（7 Section 表 + PcSampling + FieldUnitMap） | — | — |
| 通算流水图（MC² timeline） | 🟡 | 🟡（MC2/LCCL parser，依版本） | — | — | — |

## 产物 → 消费映射

| 产物 | 消费方 |
|------|--------|
| `PROF_*/mindstudio_profiler_output/op_summary_*.csv`（PipeUtilization） | verdict.py `--csv` |
| `OpBasicInfo.csv`（Task Duration / Block Dim / 频率） | facts_bridge.py（→ `task_duration_us` / `block_dim` 标量）+ L2 头开销判定 |
| `ArithmeticUtilization / L2Cache / ResourceConflictRatio` | facts_bridge.py（→ L0 规则 + L1 反证标量：`l2_hit_rate` / `vec_fp32_ratio` / `vec_total_cflt_ratio`） |
| 7 组产物根目录 | facts_bridge.py `--manifest`（一条命令产出 verdict.py `--scalar` 串；含峰值换算，读 soc-spec.yaml） |
| `PROF_*/device_*/sqlite/ai_core_op_summary.db`（task_time 表） | facts_bridge.py **DB 双源**（P1-6）：CSV OpBasicInfo 缺失时备位 task 级时长；等价查询固化在脚本（不让 LLM 拼 SQL）；DB 口径 ≠ op 级 Task Duration，禁混比 |
| npu-compute `.npu-rep` → 解包目录（7 Section CSV + Summary.jsonl） | facts_bridge.py `--source auto`（npu-compute 通道：Summary.jsonl OpInfoSummary → task_duration/block_dim；CSV 补位） |
| npu-compute 解包目录 **`summary.db`**（Roadmap #4） | facts_bridge.py DB 通道（逐指标 AVG+NULL-skip 下推，DB 是 CSV 同数据入库副本可互通）+ verdict.py `--db`（FieldUnitMap 还原列名，规则求值零改动）+ db_bench.py（性能验证） |
| `summary.db` **`PcSampling` 表**（DB 独有，无 CSV 对应物） | facts_bridge.py → `top_stall_pct` 标量（**L0_STALL_DOMINANT 的首个生产者**；仅 SIMT 指令采样，纯 SIMD 算子无此表如实 missing） |
| 旧版 .npu-rep（无 summary.db）| csv_to_db.py 升级为 DB 格式消费（列名规范化规则与 npu-compute 入库一致） |
| isolation_test.py 的 `iso_result.json` | verdict.py `--scalar t_comm_us,t_compute_us` |
| `collection_manifest.json` | 报告/复盘定位产物；防重复采集闸门 |

## 高频踩坑（一线实测）

- `--kernel-name` 仅 application 形态有效；不指定时部分版本只采第一个算子——多 kernel
  程序必须显式过滤并核对 `OpBasicInfo.csv` 全 kernel 清单（部分结果禁止调优）。
- C++ mangled 匿名命名空间符号（`_ZN12_GLOBAL__N_1...`）导致 `--kernel-name` 采集为空：
  去掉过滤全量采集，从 OpBasicInfo 抄真实 kernel 名。
- 采集日志有 libprofapi ERROR 但 CSV 正常 → 工具链噪声，**以产出检查为准**不凭 ERROR 行判失败。
- fork 多进程只采到每进程第一个 kernel → msopprof 对 fork 不完整：标 partial，改
  SHMEMI_PROF 或完整 msprof。
- 输出目录权限与软链检查会直接报错：先确认目录可写、无跨设备软链。
- 短 kernel 同时保存 host event 计时：绝对性能以 event 为主，msOpProf 用于结构诊断。
- 采集失败分流表（GLIBCXX / aic-ascend950-ops-info.ini / tiling 改动 fault 等）见
  ops-tuning-agent workflows/troubleshooting.md。
