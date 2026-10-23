# 机制卡：MC² 流水配平（TilingData 驱动的系统搜索）

> 对应判定规则：L3 各 MC² 档位的配平环节。内容：长短块排布 + AIC/AIV 分离架构 + 统一搜索算法（多卡实测沉淀）。

## AIC/AIV 分离架构与 Pattern

MC² 算子 = AIC（matmul 计算）+ AIV（UDMA/shmem 跨卡通信）分离架构，同步方向由执行
顺序决定：

| Pattern | 执行顺序 | 同步方向 |
|---------|---------|---------|
| A（如 alltoall + matmul） | AIV 先通信 → AIC 等数据 → 计算 | AIV `CrossCoreSetFlag` → AIC `CrossCoreWaitFlag` |
| B（如 matmul + alltoall） | AIC 先计算 → AIV 等完成 → BarrierAll → 通信 | AIC `NotifyComputeComplete` → AIV `WaitComputeComplete` |

配平改变的是**长短块排布**（长块 = 大 M 的计算块，短块 = 小 M 的计算块），使计算块的
耗时分布与通信块节奏互补。

## 统一搜索算法（禁止经验值交付）

输入：TilingData（baseM/baseN/baseK/usedCoreNum，长短块各自）+ 隔离测试 R 值 +
配平比 ρ = max(R, 1/R)（长块与短块的 M 大小之比）。

```
搜索空间：长块 M ∈ {候选集}，短块 M = 长块 M / ρ（对齐修正），块数切分方案
四条剪枝（可行性）：
  ① 核利用率：usedCoreNum × 块数覆盖不下降
  ② L1/L0 容量：baseM/baseN/baseK 组合不超片上预算
  ③ 膨胀下界：计算膨胀 + 通信膨胀的估算不超过收益
  ④ 对齐约束：M/N/K 对齐要求（512B / 分形对齐）
输出：Top-N 候选 → 逐个实测（tileCnt 扫描）→ 按跨 rank max 口径择优
```

**执行载体（P1-4）**：`ops-mechanism/scripts/mc2_search.py`——上述算法的脚本化：

```bash
python3 skills/ops-mechanism/scripts/mc2_search.py \
    --m 4096 --base-n 128 --base-k 64 --used-core-num 16 \
    --r-value 2.4 --soc a5 -o candidates.json
```

- 容量剪枝②读 `ops-verdict/references/soc-spec.yaml`（不硬编码）；
  对齐剪枝④分级：分形对齐 16（Cube 硬约束）剪枝，512B 对齐（搬运推荐态，
  DataCopyPad 场景可接受）降级为候选上的 warn 记录；
- 输出 Top-N 候选（每候选带块布局 / 容量占用计算 / warn）+ 全部剪枝记录
  （可审计证据：剪枝记录本身是证据）；
- 排序 = 实测顺序不是最终结论：每候选仍须逐个实测（同口径 task-based、
  跨 rank max、delta_compare 三段结论）后择优；
- **配平方案卡必须附本搜索器输出**（候选清单 + 剪枝记录）——经验值交付
  的禁令自此有执行载体。

## TilingData 依赖（硬约束 MC-1）

host 代码须打印 baseM/baseN/baseK/usedCoreNum（含长块/短块各自的 tiling data）。
未打印 → 在源码副本启用 PrintTilingData → 重新运行采集。缺这些数据核利用率计算
失真、剪枝失效。

## 长短块排布

- R < 1（通信侧慢）：短块多、长块少——让计算碎片塞进通信间隙；
- R > 1（计算侧慢）：长块多——让通信（快）在长计算块间流水；
- ρ 是理想比，实际排布按 Top-N 实测修正（估算 ≠ 实测，膨胀会吃掉理论收益）。

## 验证

每候选实测必须同口径（同一 msprof task-based 采集、同 warmup/loops、跨 rank max），
用 delta_compare 出三段结论；配平收益核算用隔离测试的 T_comm/T_compute 代入
掩盖率公式。

## 回滚

- 候选劣化（跨 rank max 口径）→ 弃用并记录（搜索的负结果也是证据）；
- tiling 改动后 device fault → 回滚确认基线可复跑 → -g 重编 + msSanitizer 定位 →
  缩小改动粒度（baseK 128→192 而非 256）→ 核对深度/容量联动字段。

## 精度风险

块切分改变累加分块顺序 → 语义变更类：边界值 + 大 shape + 异常位型验证用例必附。
