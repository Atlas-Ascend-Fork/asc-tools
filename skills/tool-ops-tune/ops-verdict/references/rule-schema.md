# 规则 Schema 规范（opstune.rules/1.0）

> 规则是判定引擎的唯一知识形态。每条规则 = 触发条件（指标+阈值）+ 反证 + 置信度 + 机制路由，
> 声明式 YAML 承载，`verdict.py` 解释执行。设计对标 Nsight Compute Guided Analysis 的
> 「规则-Section 绑定契约 + REQUIRED/OPTIONAL 分级」，阈值取自真机调优实践标定。

## 顶层字段

```yaml
schema: opstune.rules/1.0     # 固定
layer: L1                     # L3 卡间 / L2 核间 / L1 单核 / L0 指令
title: 单核流水判定
soc: [a2, a3, a5]             # 适用芯片域（过滤语义：不含目标 soc 则整包跳过）
requires_experiment:          # （可选）实验数据前置（如 MC² 隔离测试）
  kind: isolation_test
  outputs: [t_comm_us, t_compute_us, m_rows]
metrics: { ... }              # 指标词汇表（见下）
rules: [ ... ]                # 规则列表
```

## 指标词汇表（metrics）

规则-指标绑定契约的解析层。**规则只能引用词汇表中声明过的指标**——未声明的引用在加载
时应视为配置错误。

```yaml
metrics:
  mte2_ratio:                       # 指标名（规则 condition 中引用）
    source: pipe                    # pipe | scalar | derived
    columns: [aic_mte2_ratio, aiv_mte2_ratio, ai_mte2_ratio, "Mte2Utilization(%)", ...]
    normalize: ratio_to_pct         # 0~1.5 视为比例 ×100（双版本字段口径兼容）
    group: pipe                     # 参与主导 pipe 排序与串行指纹求和
  mte2_active_bw_pct_of_peak:
    source: scalar                  # 由调用方 --scalar 提供
  r_value:
    source: derived                 # 引擎内置计算（如 R = T_compute/T_comm）
```

- `source: pipe`：从 PipeUtilization.csv 逐行取候选列（按序首个可解析者），per-row 取
  aic/aiv 双列最大值，跨行算 mean/max 双统计。**判定用 mean，max 仅识别长尾**（单核峰值
  不代表全 kernel —— 实测纪律）。
- `source: scalar`：调用方提供（带宽百分比、物理核数、隔离测试值……）。缺失时引用它的
  规则按 REQUIRED/OPTIONAL 语义处理。
- `source: derived`：引擎衍生（`__max_pipe_mean`、`__pipe_sum_mean`、`__tail_ratio`、
  `__imbalance_pct`、`__two_cluster_gap_pct`、`r_value`、`__mte2_pollution_gap`、
  `dominant_time_us`、`__occupancy_pct`、`__head_overhead_pct` 等）。

**加载校验（机制化，违规启动即 exit 3）**：`verdict.py` 加载规则包后强制校验——

| # | 校验 | 说明 |
|---|------|------|
| V1 | 引用完整性 | condition / watch / counter_evidence / severity / metrics_required 引用的指标必须在词汇表或 derived 白名单内——**未声明的引用是配置错误**（不是运行时静默 unevaluated） |
| V2 | 路由目标存在 | `routing.on_hit` 的 disable_rule 目标必须存在于规则宇宙（本次加载包 + 同目录其余规则包；disable 语义是跨包的） |
| V3 | 区间无重叠 | 同一指标的多条档位类规则（如 L3 R 值五档）区间不得重叠——同一值双命中即矛盾 |
| V4 | optional 语义 | **`metrics_optional` 仅用于反证；condition 中引用的指标必须 required**。引擎对 condition 内任何 None 一律 INSUFFICIENT_DATA（三值逻辑），不存在「可选指标缺失时跳过该叶子继续求值」的降级机制——把 optional 指标写进 condition 的规则永远无法 HIT |

## 规则字段

```yaml
- id: L1_MTE2_BOUND                # 全局唯一（跨包路由引用，如 disable_rule）
  name: MTE2 搬入瓶颈               # 人读名
  priority: 10                      # 升序求值（数字小先评估）
  parent: L1_MEMORY_SATURATION      # （预留）DAG 父节点，M2 启用权重传播
  threshold_source: "[msprof-doc] >80% 直接判 / 最大且 >70%"
  metrics_required: [mte2_ratio]            # 绑定契约：缺失 → SKIPPED（不触发不误报）
  metrics_optional: [mte2_active_bw_pct_of_peak]  # 缺失 → 反证标 unevaluated；禁止进 condition（V4）
  condition:                       # 触发条件（三值逻辑：True/False/None=证据不足）
    any:
      - {metric: mte2_ratio, agg: mean, op: gt, value: 80}
      - all:
          - {metric: mte2_ratio, agg: mean, op: gt, value: 70}
          - {metric: mte2_ratio, agg: rank, op: eq, value: 1}   # rank=1 即「8 pipe 中占比最大」
  watch:                           # 未达判定线的观察档（NOT_HIT 时提示）
    condition: {metric: mte2_ratio, agg: mean, op: gt, value: 50}
    note: MTE2 占比 50~70%，暂非主导，已具备搬运优化价值
  severity:                        # 分档：critical / moderate / light
    metric: mte2_ratio             # 按指标值分档（或 default: light）
    bands: [{min: 85, level: critical, note: ...}, {min: 70, level: moderate}]
    default: light
  counter_evidence:                # 反证条目（防误判的第二道闸）
    - id: true-bandwidth-saturation
      condition: {metric: mte2_active_bw_pct_of_peak, agg: value, op: gt, value: 80}
      note: active_bw 已达理论 80%+：真带宽饱和，优化方向是数据量/复用而非搬运组织
      action: annotate             # annotate（记录）| reroute（改道机制，M2）
  confidence: high                 # high / medium / low（规则级先验）
  speedup_range: [1.2, 2.0]        # 预估加速比区间（机制路由排序依据；校准闭环回写）
  speedup_source: "🟡 首期标定值"
  mechanism_refs:                  # 机制路由入口（ops-mechanism 内文档路径）
    - ops-mechanism:references/optimize/data-copy.md
  routing:                         # （可选）命中副作用
    on_hit: [disable_rule, L1_MTE2_BOUND]   # 禁用指定规则（跨包生效）
    reroute_to: ops-mechanism:references/optimize/mc2.md
```

## 条件语义（三值逻辑）

| 节点 | True | False | None（证据不足） |
|------|------|-------|----------------|
| `all: [...]` | 全部 True | 任一 False | 有 None 无 False |
| `any: [...]` | 任一 True | 全部 False | 有 None 无 True |

- **None 不冒充 False**：条件无法评估时规则置 `INSUFFICIENT_DATA` 并提示补采，绝不因
  数据缺失而判「无 bound」。
- `value` 支持复合引用：`{metric: time_mean_us, factor: 1.25}`（另一指标 × 系数）。
- `op` 集合：`gt / gte / lt / lte / eq / between [lo,hi] / gt_ratio / lt_ratio`。

## 路由副作用

`routing.on_hit` 在命中时生效（按 priority 顺序，跨规则包传播）：

- `disable_rule`：禁用后续同批规则 —— 用例：`L3_MTE2_POLLUTED` 命中后禁用
  `L1_MTE2_BOUND`（MTE2 ratio 被通信等待污染，单核访存优化无效，改道通信掩盖策略）。
- `reroute_to`：改写 finding 的机制路由目标（M2 启用 DAG 权重传播后扩展为 GLOBAL/LOCAL
  加速比语义，对标 NCU 父子规则）。

## 扩展流程（贡献一条新规则）

1. 在对应层规则包 YAML 追加规则（阈值必须带 `threshold_source`；无来源实测支撑的标 🟡）。
2. 在 `fixtures/` 造已知答案的输入（CSV 或标量），在 `evals/run_evals.py` 加断言用例
   （正例 + 至少一个反例/边界）。
3. `python3 evals/run_evals.py` 全绿 → 合入。
4. 知识组评审：阈值来源、soc 适用域、反证完整性（缺反证的 BOUND 类规则不允许合入）。

## 纪律（实测教训沉淀）

- **判定只出自规则**：任何「看起来像 bound」但不在规则库中的结论，Agent 不得输出为判定，
  只能作为 `INSUFFICIENT_DATA` 下的假设并注明缺失指标。
- **阈值不可无源合入**：`threshold_source` 为空或 🟡 的规则在评审中降级为 watch 档。
- **跨芯片域**：规则包 `soc` 过滤是硬边界；A2/A3/A5 阈值差异通过分包或（M2）规则内
  soc 覆写承载，禁止静默复用。
