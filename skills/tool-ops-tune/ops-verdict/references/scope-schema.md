# scope.json 契约（opstune.scope/1.0）

> Phase 0「意图与边界」的输出载体，调优计划的口径真源。字段名与
> `snapshot.py` / `delta_compare.py` 的 HARD_KEYS **严格同名同义**——口径互锁
> （12 HARD + 2 机制变量）的上游真源，不同执行者生成的 scope.json 不再漂移。

## 字段清单

| 字段 | 来源 | 说明 |
|------|------|------|
| `schema` | 固定 | `opstune.scope/1.0` |
| `operator` | Phase 0 | 算子名（人读） |
| `op` | HARD | 与 snapshot `--op` 同义（口径指纹字段） |
| `soc` | HARD | `a2` / `a3` / `a5` |
| `mode` | HARD | `board` / `sim` |
| `timing_method` | HARD | 五选一：`event` / `sim` / `msprof` / `msprof_task_duration` / `profiler_summary` |
| `baseline_kind` | HARD | `system`（对标系统算子）/ `self_before_after`（自前后对比） |
| `cann_version` | HARD | CANN 版本号 |
| `device_id` | HARD | 采集设备 ID |
| `shape` | HARD | 代表 case 的 shape |
| `dtype` | HARD | 数据类型 |
| `format` | HARD | `ND` / `NZ` / … |
| `warmup` | HARD | 冷跑剔除轮数 |
| `repeat` | HARD | 稳态采样轮数 |
| `tiling_key` | 机制变量 | 优化本身可能改变（WARN 级记录） |
| `block_dim` | 机制变量 | 同上 |
| `scene` | Phase 0 | `single` / `mc2` / `fa` / `regression` / `qa`（场景分类判据见 task-prompts Step 0） |
| `cases_csv` | 输入 | 测试用例文件路径 |
| `code_dir` | 输入 | 算子源码目录 |
| `performance_goal` | 输入 | 性能目标（可为 null） |
| `max_rounds` | 输入 | 轮数上限 |
| `precision_gate` | Phase 0 预检 | `{total, pass, fail, tolerance}`——fail≠0 禁止进入调优 |
| `generated_at` | 生成时间 | ISO 8601 |

## 最小示例

```json
{
  "schema": "opstune.scope/1.0",
  "operator": "gelu",
  "op": "gelu", "soc": "a5", "mode": "board",
  "timing_method": "event", "baseline_kind": "self_before_after",
  "cann_version": "8.3.0", "device_id": "0",
  "shape": "8448x4096", "dtype": "fp16", "format": "ND",
  "warmup": 5, "repeat": 20,
  "tiling_key": null, "block_dim": null,
  "scene": "single",
  "cases_csv": "cases/gelu_cases.csv",
  "code_dir": "/path/to/gelu_project",
  "performance_goal": "kernel_avg_us < 100",
  "max_rounds": 1,
  "precision_gate": {"total": 12, "pass": 12, "fail": 0, "tolerance": "工程现有 ut 容差"},
  "generated_at": "2026-10-14T12:00:00"
}
```

## 纪律

- 12 HARD 字段与 `delta_compare.py` 的 `HARD_KEYS` 一一对应：任一变化必须重建基线
  （跨口径比较禁令，A4 铁律）。
- `precision_gate.fail != 0` 的 scope 禁止进入 Step 1（第一道精度门禁）。
