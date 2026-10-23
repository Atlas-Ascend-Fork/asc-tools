---
name: ops-collect
description: NPU 性能采集编排。统一采集入口：上板（msprof 7 组指标 / quick / MC² task-based 多 rank）、MC² 隔离测试编排（T_comm/T_compute → R 值）、采集块 manifest 契约（已有 manifest 禁止重复采集）。负责把「能采但采不到」变成「按路由采对」，并保证采集产物可直接喂 ops-verdict 判定引擎。触发：性能采集、profiling、基线数据采集、MC² 采集、隔离测试、补采指标时使用。
---

# 性能采集编排（collect）

> 采集的职责边界：只负责「采对、采全、口径留痕」，不做判定（ops-verdict）、不做解释
>（ops-mechanism）。底层工具全部复用 CANN 官方（msprof / msprof op / msopprof / cannsim），
> 本 SKILL 不重做采集器，只做模式路由与契约落盘。

## 采集模式路由（先探测本机帮助，再选命令）

```
用户输入（可执行文件 + 用例）
  │
  ├─ MC² / fork 多 rank 算子？（fork 多子进程绑卡 + SHMEM UDMA / BarrierAll / CrossCoreFlag）
  │    ├─ 是 → collect_board.sh --mode mc2
  │    │        msprof --aic-mode=task-based（msprof op 对 fork 行为未定义，数据不可靠，禁用）
  │    │        MC² 调优另需隔离测试 → isolation_test.py（T_comm/T_compute → R）
  │    └─ 否 → 单卡路径
  │         ├─ 本机 msprof op --help 可用 → msprof op 采集（含 Roofline/KernelScale 等能力按本机帮助）
  │         └─ 仅 msprof 可用 → collect_board.sh --mode standard（7 组 aic-metrics 顺序采集）
  │
  └─ 无 NPU / 需指令级 → cannsim 仿真路径（A5 单卡 AI Core；MC²/HCCL 不支持）
```

三条硬规则：

1. **先探测再选命令**：所有可选参数以本机 `--help` 为门禁，禁止把一台 A5 beta 环境的
   参数复制到 A2/A3（跨版本绝对规则禁令）。
2. **已有 `collection_manifest.json` 的目录禁止重复采集**：只允许重解析（verdict.py）
   或换新目录重采。重复 replay 破坏稳态且浪费设备时间。
3. **MC² 禁 host 侧计时**：std::chrono/gettimeofday 含 launch 开销且精度不足，一切
   性能数据必须出自 msprof `op_summary` 的 `Task Duration(us)`。

## 脚本清单

| 脚本 | 职责 |
|------|------|
| `scripts/collect_board.sh` | 上板采集统一入口（standard / quick / timing / mc2 四模式）+ manifest 落盘；timing 模式跑 event 计时 harness 并归档 result.json（P0-2） |
| `scripts/collect_cases.py` | **逐 case 采集编排 + 标杆双路径**（P0-1，A1 铁律工具化）：cases.csv + exe [--ref-exe] → 逐 case×variant 调 collect_board → `perf_data.csv`（batch_pipeline `--from-collect` 直接输入）+ 逐 case manifest 归档；case 间串行同卡同口径；防重采闸门（已有 manifest 跳过）；`--cases-subset` 定向补采；failed/partial 如实进 collect_summary（伪 Green 防护） |
| `scripts/isolation_test.py` | MC² 隔离测试编排：T_comm/T_compute（逐 rank 稳态均值，跨 rank max）+ mte2_ratio_full/pure / rank_imbalance / mask_ratio → R 值与 --scalar 串 → 喂 verdict |
| `scripts/facts_bridge.py` | msprof 7 组产物 → verdict `--scalar` 串（G2 桥接）：L2Cache/Arithmetic/ResourceConflict/OpBasicInfo 提取 + soc-spec 峰值换算，产出 `mte2_active_bw_pct_of_peak` / `l2_hit_rate` / `vec_fp32_ratio` 等——L0 规则与 L1 反证的标量生产者；missing 列表即补采闭环真源；**DB 双源**（P1-6）：CSV OpBasicInfo 缺失时 sqlite（`ai_core_op_summary.db` task_time）备位 task 级时长（等价查询固化在脚本，ns→us 换算 + DB 口径注记防混比；`--no-db` 关闭） |
| `scripts/jsonl_to_cases.py` | **JSONL 用例适配**（P1-5，G11）：cannbot 生态 JSONL 用例（torch-ops-profiler 规范）→ cases.csv（A1 铁律输入契约）；转换规则全落脚本（dtype 归一 / attrs 拼接 / CSV 转义），歧义行 skipped 列明原因不猜测 |
| `scripts/collect_sim.sh` | **仿真路径采集编排**（P1-1，G7）：cannsim record（精度+性能仿真）→ report（trace_core*.json）→ mode=sim 的 collection_manifest 归档；cannsim 探测门禁（不可用明确报错）；sim 数字与上板禁混比（snapshot timing_method=sim 专用） |

**自动化分级**（如实宣称）：
- 宿主程序自带模式开关（环境变量）→ **全自动**（env 模式：脚本注入开关、三轮采集、
  产出完整 verdict --scalar 串）；
- 宿主无开关（真实 MC² 工程常态）→ **半自动 from-prof**：Agent 在源码副本上注释
  通信/计算 process 并手工 msprof 后，把两份产物目录交给 `--switch-mode from-prof`
  收敛（开关参数支持 K=V 形式注入自定义开关）。
- 源码副本定向注释自动化（识别通信/计算 process 注释点）为 26-1 前置工作项。

## 采集块 manifest 契约（opstune.collection/1.0）

每次采集输出 `collection_manifest.json` —— 采集块的唯一真源：

```json
{
  "mode": "standard",
  "exe": "./build/demo", "args": "8448 4096 4096",
  "device_id": "1", "ranks": 1, "kernel_name": null,
  "blocks": [
    {"name": "PipeUtilization", "status": "ok", "path": "PROF_PipeUtilization_20261014_..."},
    {"name": "L2Cache", "status": "failed", "path": "...", "note": "msprof 退出非 0"}
  ]
}
```

- `status`：`ok / partial / failed / skipped`（采集块状态机）。
- 判定引擎按 manifest 定位 CSV；failed 块在 verdict 侧表现为 REQUIRED 缺失 → SKIPPED
  （绑定契约兜底：采不到就不判，不静默编造）。
- **禁止伪 Green**：任何块失败必须在报告中如实呈现，不得从产物清单里悄悄抹掉。

## MC² 隔离测试纪律（isolation_test.py 编排，纪律由本节承载）

1. 隔离测试在**源码副本**进行（`{code_dir}_diag/`），注释通信或计算 process，测完丢弃
   代码副本、保留 profiling 产物。
2. tileCnt=1（无流水线）条件下分别测纯通信 / 纯计算。
3. 稳态：循环 ≥10 次，前 5 warm-up，取后 5 均值；**跨 rank 取 max**（木桶效应——所有
   rank 经 BarrierAll/CrossCoreFlag 同步，整体性能由最慢 rank 决定）。
4. R = T_compute / T_comm → 五档 bound（verdict.py L3 规则包消费）。
5. 掩盖率 =（融合前耗时 − 融合后耗时）/ min(T_comm, T_compute)，上限 1.0。

## 计时口径（供 snapshot.py --timing-method 选择）

| 情形 | 规则 |
|------|------|
| 工程自带 benchmark | 视为正式 benchmark，按优先级采信 |
| 无计时设施 | **拷计时 harness 货架**（ops-family 子域 `references/common/timing_harness/`，C++ aclrtEvent 与 torch event 两模板，随算子族 PR 交付；`collect_board.sh --mode timing` 直跑归档）——属测量设施而非优化变量，原基线源码先备份 |
| 长 kernel（ms 级）无 event | 允许 Task Duration 作基线，报告注明口径 |
| 短 kernel（µs 级） | launch 开销掩盖收益：harness 模板内置 stream 内多 launch 单 event 对计时；仍不可归因用 profiler device duration 辅助并注明 |
| 冷跑/稳态 | 首跑含初始化与缓存预热（实测可差 25%）：固定 warmup 剔除，稳态 ≥3 次取中位（harness 模板已固化此结构） |
| profiling 构建 | 保持 Release；源码映射只加调试信息，禁 -O0（优化级别变化使前后数据不可比） |
| 频率漂移（DVFS） | **可选锁频**（P2-2）：`npu-smi set -t freq-lock`（需 root；采集期固定频率消除 DVFS 爬升噪声）。**副作用如实标注**：锁频数字与出厂默认频率下的性能不可直接外推，报告须注明「锁频口径」；无 root 权限时按 warmup 爬升 + 多轮稳态中位数缓解（本仓默认路径，不强制锁频） |

## 设备选择（P2-2/G20 ①）

`collect_board.sh` 选卡逻辑：**结构化解析优先**——`npu-smi info -t usages -i <id>`
的 `Aicore Usage Rate(%)` 字段判定（< 50% 视为可用），对 npu-smi 输出格式变化
免疫；结构化解析失败（版本差异）才退化到 `npu-smi info` 表字面匹配并 stderr
提示。选卡后注入 `ASCEND_RT_VISIBLE_DEVICES`（口径指纹 device_id 源头保真）。

## 参考文档

| 文件 | 内容 |
|------|------|
| [`references/tool-routing.md`](references/tool-routing.md) | msprof / msprof op / msopprof / cannsim / torch_npu.profiler 选择决策树与跨版本能力矩阵 |
