# 机制卡：Ascend C Tiling 优化（切分与多核负载）

> 对应判定规则：L2_CORE_TAIL、L2_IMBALANCE_*、L2_UNDERUTILIZATION、L1_MTE2_BOUND
>（tile 尺寸侧）。内容沉淀自 tiling 调优一线实践与各族理论建模沉淀。
> **DSL 适用域（P2-5/G19）**：本卡实施细节以 **Ascend C** 为准；Triton / CATLASS / TileLang 的等价原语与实施差异见 [index.md 跨 DSL 原语对照表](index.md#跨-dsl-原语对照六原语--dsl)——跨 DSL 实施时以该表为路由、对应 DSL 专项文档为深度参考，不按本卡字面照搬。

## 机制清单

| 场景 | 机制 | 说明 |
|------|------|------|
| 切核不足 | Block Dim 提升 | 「最低垂果实」：4 核→20 核案例利用率 10%→50%；但 Task <25us 时增核放大头开销，须权衡。**候选参数不再经验拍**：`ops-family/scripts/tiling_candidates.py` 按 soc-spec 容量约束产出候选表 + 逐候选容量占用（P0-5） |
| 核间长尾 | 尾块均衡 | 数据总量不整除核数时尾块分散到更多核（swat：蛇形行窗口 + 尾块合并 + 末轮二维再切分） |
| 长尾持续 | block swat / 尾轮均衡 | 两簇检测（前后半段核均值差 ≥2%）提示 NUMA/L2 slice 偏斜时优先 |
| 搬运次数多 | 增大 tile | mte2_instructions 过大时（与 UB 预算联动；候选表已附 UB 占用计算） |
| MN 欠并行 | streamk（SK/DP+SK） | totalBlockCnt < aicNum + K 长：切 K 到空闲核，AIV 归约（matmul 族） |
| 尾碎片不均 | swat | MN 够并行（>aicNum 数倍）但尾碎片负载不均 |

> MC² 配平候选（四剪枝 + Top-N）的执行载体为 `mc2_search.py`（P1-4，M2 交付）。

## Tiling 理论建模（多机制联动时的前置）

卡间切分 ↔ 核间切分 ↔ 单核 Buffer 规划三层联动（跨层耦合）：改一个 baseM 同时影响
通信粒度、核利用率、L1/L0 容量。多机制候选冲突时先做 tiling 建模再选单变量——

```
Step1 输出：卡间切分 / 多核切分 / 单核切分（Cube: L1+L0 split；Vec: UB split）
        / Buffer 规划（L1/L0/UB 分层）/ 分支场景覆盖（dtype/shape/对齐）
```

各族理论模型与 fallback：`ops-family`（matmul/mc2/reduce/elementwise/broadcast/
conversion/convolution/fa/sort 路由表）。**fa 族当前仅 fallback 兜底（已知缺口）**，
FA 类调优按无同型案例降级策略执行并沉淀案例。

## 验证

L2 规则复跑（__tail_ratio / __imbalance_pct / row_count 变化）+ 同口径性能；
tiling 改动后 device fault 处置：①回滚确认基线可复跑 ②-g 重编 + msSanitizer 定位
③缩小改动粒度 ④核对深度/容量联动字段。

## 回滚

长尾加剧 / 利用率未升 / 精度失败 → revert。切分类改动可能改变 tiling_key——
delta_compare 默认 WARN 不阻断（MECHANISM_KEYS 语义）。

## 精度风险

切分与分核默认语义保持；改变归约分块顺序 → 语义变更类（附验证用例）。

## 模板资产

`ops-family:references/matmul/guide.md`（六策略互斥判定树）、各族 guide。
