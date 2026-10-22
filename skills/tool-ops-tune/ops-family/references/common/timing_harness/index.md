# timing_harness — 计时口径货架（REVIEW2 G4 / 整改 P0-2）

> snapshot.py `--kernel-avg-us` 的第一生产者。此前的状态：口径纪律只在
> ops-collect SKILL.md 计时口径表（文字），无自带 benchmark 的工程要靠
> agent 手写计时代码——短 kernel launch 开销掩盖收益是最高频的坑。
> 本目录把「warmup 剔除 + 稳态 ≥3 取中位 + 多 launch 单 event」从纪律变成拷模板。

## 模板清单

| 模板 | 适用 | 产物 |
|------|------|------|
| [`timing_harness_aclrt_event.cpp`](timing_harness_aclrt_event.cpp) | C++ Kernel 直调工程（无自带 benchmark 时的标配） | `result.json`（opstune.timing/1.0：kernel_avg_us + p50/p95/max + 逐轮值） |
| [`timing_harness_torch_event.py`](timing_harness_torch_event.py) | torch_npu 侧（torch.ops 调用形态 / torch 工程对标） | 同上 |

## 使用纪律（模板拷贝纪律同款：直接拷贝 + 最小适配）

1. 拷模板入工程（`test/` 或根目录），只改 `[MODIFY]` 标注点
   （被测调用 + 设备/链接），**计时结构（warmup/repeat/多 launch 单 event/取中位）
   不许改**——改了口径，前后对比全部失效；
2. 输入输出分配放 `setup()`（循环外），`run_once()` 只留 launch；
3. `warmup/repeat` 是 HARD 口径字段：与 scope.json 一致；噪声带内复测时
   repeat 翻倍并**同步重采基线**（delta_compare 会拒绝单侧口径漂移）；
4. 产物 `result.json` 的数值直接喂 snapshot.py：
   `--kernel-avg-us <avg> --timing-method event --kernel-p50-us ... --kernel-p95-us ...`。

## 何时不用本货架

- 工程自带正式 benchmark → 视为正式口径，按优先级采信（ops-collect 计时口径表第 1 行）；
- 长 kernel（ms 级）无 event 设施 → 允许 Task Duration 作基线，
  `--timing-method msprof_task_duration` 并在报告注明（表第 3 行）；
- torch_npu.profiler 路径 → 见 tool-routing 决策树⑤（schedule 固定 5/5）。

## 复制目录带入优化副本（perf-implementer 联动）

perf-implementer「复制目录」步骤把本 harness 视为**测量设施**带入优化副本
（非优化变量）——基线与优化侧同一 harness、同一 warmup/repeat，口径两侧同源；
对照 ops-collect SKILL.md「允许在产物目录工程副本补 aclrtEvent harness
（属测量设施而非优化变量），原基线源码先备份」。
