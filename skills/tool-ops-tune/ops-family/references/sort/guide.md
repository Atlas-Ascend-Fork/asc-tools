# Sort 族指南（guide）——TopK/Sort/ArgSort M-way 归并排序

> **来源（蒸馏）**：Ascend C Tiling 设计方法论·sort 分册 +
> `ascendc-perf-optimize/references/tiling/sort/fallback/`（tiling-flow /
> tiling-fields / sort_tiling.py 参考实现）——此前本族 guide 仅 4 行骨架
>（radix_sort.md 为另一实现路线的实质文档）。

## 族定位

Top-K 排序算子的主流实现是 **M-way 归并排序**（`MrgSort` API，M 通常 = 4）：
Divide（按 UB 容量切 tile）→ 各核并行排序 → 多轮归并（Conquer）。
radix_sort 路线（见 [radix_sort.md](radix_sort.md)）适用于整数键/特定位宽场景。

## 核心约束：UB 每元素占用

Sort API 要求 float 输入 + proposal（value+index 8B）输出，单元素 UB 占用：

```
sortBytesPerElem = sizeof(dtype) + sizeof(float) + sizeof(uint32_t)
                   + PROPOSAL_SIZE(8) + concatTmpPerElem + sortTmpPerElem
```

- fp32 输入可省 Cast buffer（≈32B/元素）；fp16/bf16 需 Cast（≈34B/元素）；
- **concatTmp / sortTmp 必须从 API 获取**（`GetConcatTmpSize` /
  `GetSortTmpSize`），禁手动估算（平台相关）；
- tileSize 上界：`ubSize / sortBytesPerElem`，对齐 32（`TOPK_SORT_NUM`）；
  工程实践通常取 **4096**（实测性能最优值，留 UB 余量给对齐填充与 API 峰值）。

## 三 Pattern 路由（决策树）

```
tileSize = min(ubSize / sortBytesPerElem, 4096)，对齐 32

N ≤ tileSize                    → Pattern A：单核排序（GM→UB→Sort→GM）
tileSize < N ≤ tileSize×coreNum → Pattern B：多核一级归并（各核 ≤1 tile，
                                   排序后直接跨核归并）
N > tileSize×coreNum            → Pattern C：多核两级归并（每核 >1 tile）
```

## Pattern C 四阶段架构

| Phase | 职责 | 核参与 | 数据流 |
|-------|------|:----:|--------|
| 1 | 各核并行 tile 排序 | 全核 | GM→UB→Cast+ArithProgression+Concat→Sort→workspace |
| 2 | 核内多 tile 归并 | 全核（**无同步**） | workspace→UB→MrgSort→workspace |
| 3 | 跨核归并（路数→≤M） | 递减 | workspace→UB→MrgSort→workspace（每轮 SyncAll） |
| 4 | Core0 最终归并 + 输出 | 1 核 | workspace→UB→Extract 分离 value/index→GM |

**核内先归并的关键设计**：跨核归并每轮需 SyncAll 等所有核——若跳过 Phase 2
直接把 totalTiles 路跨核归并，轮次 = ceil(log_M(totalTiles))，每轮一次
SyncAll 不可接受。先核内归并把路数从 totalTiles 降到 coreNum。

## Tile 切分公式

```
totalTiles      = CeilDiv(N, tileSize)
frontCoreTiles  = CeilDiv(totalTiles, coreNum)
usedCore        = CeilDiv(totalTiles, frontCoreTiles)
lastCoreTiles   = totalTiles - (usedCore-1) × frontCoreTiles
lastTileSize    = N - tileSize × (totalTiles-1)
```

## 判定规则映射（verdict 命中 → 本族机制）

| verdict 命中 | Sort 场景解读 | 处置 |
|-------------|--------------|------|
| L1_VEC_BOUND | Sort/MrgSort 向量指令主导（Phase 1/2 热点） | 检查 tileSize（4096 最优）；dtype Cast 是否可省 |
| L1_SCALAR_BOUND / 头开销 | tile 过小或核内归并循环过多 | 增大 tileSize；核对 Phase 2 归并轮次 |
| L2_CORE_TAIL / IMBALANCE | lastCoreTiles 与 frontCoreTiles 差距大 | 尾核 tile 再均分（tiling 字段见参考实现） |
| L2_UNDERUTILIZATION | totalTiles < coreNum | Pattern B/C 误选——回决策树核对 |

## 参考实现

Python 参考实现（Pattern A/B/C 路由 + 两级归并 + TilingData 字段）：
Ascend C 性能优化实践·sort tiling 参考脚本
（433 行，字段语义见同目录 tiling-fields.md）——移植进本仓 tiling_candidates.py
时按契约方式推导，不直接复制。

## 模板状态（如实标注）

本族无可拷贝 kernel 模板（模板数 0）——实施按本文决策树 + radix_sort.md
（radix 路线）推进；Sort/MrgSort/GetSortTmpSize 等 API 用法以 asc-devkit
文档为准。
