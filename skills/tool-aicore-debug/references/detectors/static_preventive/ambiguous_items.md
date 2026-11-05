# ambiguous_items — 歧义项（灰色地带）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | ambiguous_items |
| 异常类型 | 已测试未直接命中 / 软件屏蔽（灰色地带） |
| 错误码 | 无 / 不确定 |
| 类别 | static_preventive |
| 性质 | 探索型 |
| 覆盖条目 | 歧义项 5：L102 / L109 / L110 / L7 / L8（不计入 57 条确认不可复现） |
| 状态 | **未确认**（"已测试未直接命中"或"软件屏蔽"灰色地带） |
| 报错模式 | 未知 / OOB 兜底 |

## 候选检测方向

| 文档行 | 错误特性 | AscendC API 状态 | 核查结论 | 建议归类 |
|--------|---------|-----------------|---------|---------|
| L102 | Atomic 访问 dirty cacheline | **API 存在**（`c_api/atomic/atomic.h` 受 `2201` 守卫；SIMT `asc_atomic_add` 等；basic_api `AtomicAdd/Max/Min/Cas/Exch`） | API 可用，但 dirty cacheline 命中需特定 cache coherence 序列。v11 `atomic_dirty_cacheline` 实测 → OOB 兜底，未命中目标错误码 | C 类"已测试未直接命中"——可继续探索 |
| L109 | su_ccu_neg_sqrt_fp_t0 | 标量浮点 sqrt，"建议软件屏蔽" | 硬件能检出但 RTS/驱动层清除错误标志。v1-v3 实测 → OOB 兜底 | C 类"软件屏蔽型"——硬件检出但软件不报 |
| L110 | su_ccu_div0_fp_t0 | 标量浮点除零，"建议软件屏蔽" | 同上（硬件检出但软件不报） | C 类"软件屏蔽型" |
| L7 | CUBE HSET counter 下溢 | "ASCENDC没有对外封装" | 同 L59 根因（无直接 HSET/HWAIT API）。v11 `hset_counter_overflow` 实测 → OOB 兜底 | 可并入 C-5（当前保守不计） |
| L8 | CUBE HSET counter 上溢 | 同上 | 同上 | 可并入 C-5（当前保守不计） |

> **注**：L7/L8 与 C-5 的 L59 属同一根因（HSET/HWAIT counter 无对外 API），但触发位置不同（CUBE 内部 vs MTE→CUBE 通道）。若将 L7/L8 也计入不可复现，总量为 **59 条**。当前保守起见仅计 L59，L7/L8 标注于本节供参考。

## 关联异常

- **atomic_dirty_cacheline**（runtime）：L102 的实测用例（OOB 兜底）
- **scalar_fp_neg_sqrt** / **scalar_fp_div0**（runtime）：L109/L110 的语义关联
- **hset_counter_overflow**（runtime）：L7/L8 的同族表现（OOB 兜底）
- **compiler_guaranteed**（static_preventive）：L7/L8 与 L59 同根因（C-5）

## 检测执行指引

- **未确认条目**：仅记录候选方向，不参与规则匹配
- diagnose Step 0：不主动输出；仅在用户显式查询"歧义项" / "灰色地带"时返回候选分析
- 优先级：仅查询时可见
