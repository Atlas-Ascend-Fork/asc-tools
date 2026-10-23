# MC² 族性能优化指南（3 策略）

> A 型。适用 DAV_3510。适用于 MC² 通算融合族所有变体：`matmul_all_reduce`、
> `allgather_matmul`、`matmul_reducescatter`、`alltoall_matmul` 等。
>
> 机制决策细节（Bound 判定 / R 值五档 / 配平搜索 / 膨胀分析）见 ops-mechanism
> `optimize/mc2.md` 与 `optimize/mc2-balancing.md`——本族 guide 只维护策略-文档映射
> 与族特有约束。

## 策略表

| 策略 | 优先级 | 适用场景 | 核心手段 | 设计文档 |
|------|--------|---------|---------|---------|
| **local_matmul** | 首选 | 所有 MC² 通算融合算子 | 将本 rank 数据的 matmul 从通信块循环中分离，前置/后置独立执行以与通信重叠 | [local_matmul_design.md](local_matmul_design.md) |
| **pipeline_balancing** | 中 | 所有 MC² 算子（在 Local Matmul 基础上叠加） | 按 Bound 类型确定长短块排布，**TilingData 驱动的系统搜索**（四条剪枝 + Top-N 候选实测）选最优切分 | [pipeline_balancing_design.md](pipeline_balancing_design.md) |
| **matmul 计算效率优化** | 最低 | 仅计算 Bound（正交可叠加） | 参考 matmul 族优化策略（pingpong/streamk/fullload 等） | [../matmul/guide.md](../matmul/guide.md) |

> Local Matmul 不依赖 Bound 判定，推荐优先实现。流水配平须先打印 TilingData
> (baseM/baseN/usedCoreNum) 并执行隔离测试获取 R 值（isolation_test.py），再通过系统
> 搜索算法（非经验值）确定最优 longMSize。三者可结合使用。

## 融合模式分类

| 融合模式 | 执行顺序 | 典型算子 |
|---------|---------|---------|
| TP 权重聚合 | 通信→计算 | allgather_matmul |
| TP 输出分发 | 计算→通信 | matmul_reducescatter |
| 全局归约 | 计算→通信 | matmul_all_reduce |
| EP 专家分发 | 通信→计算 | alltoallv_grouped_matmul |
| EP 反向分发 | 计算→通信 | grouped_matmul_alltoallv |

> 纯通信算子（无计算融合）不属于 MC² 族，不适用本族优化策略。

## 族特有约束

- 前置数据：TilingData（PrintTilingData 补采）+ 隔离测试 R 值（`ops-collect` 的 isolation_test.py）；
- 采集：msprof task-based、循环 ≥10 前 5 warm-up、**跨 rank 取 max**、禁 host 计时；
- 指标核算：每方案必出加速比 + 掩盖率 =（融合前耗时 − 融合后耗时）/ min(T_comm, T_compute)；
- 抖动诊断：时间维（同卡多次）× 空间维（不同卡同轮）双维分析，异常轮剔除规则
  （首跑 >3× 均值）；
- 硬约束：缺 TilingData 不得做配平分析；配平方案必须系统搜索（四剪枝 + Top-N 实测），
  禁止 1-2 个经验值交付；隔离测试在源码副本进行。
