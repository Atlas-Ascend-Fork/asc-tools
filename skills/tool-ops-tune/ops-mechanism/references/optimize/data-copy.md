# 机制卡：数据搬运优化（MTE2/MTE3 —— 真假 bound 二分）

> 对应判定规则：L1_MTE2_BOUND、L1_MTE3_BOUND、L1_FIXP_BOUND。
> 核心方法：**先分真假 bound，再选机制**——真带宽饱和优化数据量/复用；假 bound（busy 高
> 但带宽低）优化搬运组织。判定引擎的反证条目已给出方向，机制卡只做实施。
> **DSL 适用域（P2-5/G19）**：本卡实施细节以 **Ascend C** 为准；Triton / CATLASS / TileLang 的等价原语与实施差异见 [index.md 跨 DSL 原语对照表](index.md#跨-dsl-原语对照六原语--dsl)——跨 DSL 实施时以该表为路由、对应 DSL 专项文档为深度参考，不按本卡字面照搬。

## 原理与二分

```
理论 MTE2 耗时 = GM_to_UB_datas(KB) × 1024 / GM 峰值带宽
实际 MTE2 耗时 = ai*_mte2_time(us)

实际 ≈ 理论（差 <20%，或 active_bw ≥ 峰值 80%）→ 真带宽饱和：
  机制方向 = 削减数据量（复用/驻留/更小 dtype）与减少搬运次数（增大 tile）
实际 >> 理论（active_bw < 40% 峰值）→ 假 bound（搬运组织问题）：
  机制方向 = 合并小搬运 / 对齐 / 缓解同步
```

## 机制清单

| # | 机制 | 适用条件 | 检查方式 |
|---|------|---------|---------|
| 1 | 增大单次搬运量 | 单次 <16KB | GM_to_UB_datas / mte2_instructions |
| 2 | 512B 地址对齐 | fixpipe_ratio >5~15% | PipeUtilization fixpipe_ratio |
| 3 | 合并小搬运 | 多次 <20KB 小块 → 合并成一次大 MTE2（scaleKL1 = SCALE_L1_BUFFER_NUM × kL1） | matmul scale_coalescing 实践 |
| 4 | L2 CacheMode | L2 hit <50% | L2Cache.csv；只读一次的数据 CACHE_MODE_DISABLE 防污染 |
| 5 | UB/TBuf 常驻复用 | 多 tile 重复从 GM 搬同一数据 | Bank 冲突规避与常驻联动设计 |
| 6 | Double Buffer | MTE 与计算串行 | 转 pipeline.md |
| 7 | 增大 tile | 搬运次数过多 | mte2_instructions 过大 |
| 8 | 避免同地址多核访问 | 各核 mte2_time 差异大 | 跨核共享读改广播/分片 |
| 9 | L2 复用（带宽未打满时） | tile 调大使数据驻留 L2；**相邻核处理相邻数据**共享缓存行 | 实际带宽/峰值 <80% 且有复用 |
| 10 | 基本块选型（stride 消除） | stride 搬移效率低于连续搬移——重构数据布局/选连续大粒度搬移指令 | 搬移指令序列含高 stride |
| 11 | 原地消费 | UB 内数据可直接消费的不搬出再搬入 | 搬入搬出成对出现且无中间消费方 |

## 适用前提

- verdict 命中 L1_MTE2_BOUND（severity 分档决定投入深度）；
- **反证条目必须先读**：true-bandwidth-saturation 命中 → 只走 1/4/5/7（数据量侧）；
  fake-mte2-small-packet 命中 → 优先 2/3/8（组织侧）；
- MC² 算子先过 L3_MTE2_POLLUTED：污染为 true 时本卡全部机制无效，转 mc2.md 通信掩盖。

## 验证

```bash
# 同口径性能 + 搬运指标复采
# 期望：mte2_active_bw 上升（组织侧修复）或 mte2_instructions 下降（数据量侧修复）
python3 ../ops-verdict/scripts/verdict.py --rules ../ops-verdict/rules/L1-single-core.yaml \
    --csv <new.csv> --scalar mte2_active_bw_pct_of_peak=<复测值> --soc <soc> -o round-N.verdict.json
```

## 回滚条件

噪声带内 / 精度失败 / L2 命中率反而下降（CacheMode 误用污染）→ revert。

## 精度风险

数据搬运类机制默认语义保持；改 dtype（如 fp16 计算）属语义变更，须精度代价标注。

## 模板资产

- `ops-family:references/common/datacopy_optimization_design.md`（非对齐/小批量多搬；设计参考，无货架代码）
- `ops-family:references/common/ub_resident_design.md`（常驻复用 + Bank 规避；设计参考，无货架代码）
- matmul 族 fullload / scale_coalescing / mte2_preload：`ops-family:references/matmul/guide.md`
  （均为设计参考——matmul 族当前无货架模板代码）
- 可拷贝货架模板见 conversion（11 个 transpose .template）与 broadcast（code/ 5 个
  .h/.cpp）两族；reduce 族 Softmax 模板 ×8（`references/reduce/templates/dav310/`）
