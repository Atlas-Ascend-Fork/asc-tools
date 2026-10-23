# Elementwise 族指南（guide）

> P1-7③（G14）：elementwise 此前为 🟡 骨架（仅 index + 2 篇设计文档，无 guide
> 路由层）——本 guide 补路由，设计文档补实施深度。

## 族特点

所有输入输出同 shape、无跨元素依赖——瓶颈形态高度一致：**memory-bound**（MTE2
搬入 / MTE3 搬出主导），或小 shape 下的 launch/头开销主导。判定入口：
`L1_MTE2_BOUND` / `L1_MTE3_BOUND` / `L1_SERIAL_NO_OVERLAP` / `L2_UNDERUTILIZATION`。

## 优化路由（按 verdict 命中）

| verdict 命中 | 首选机制 | 文档 |
|-------------|---------|------|
| L1_SERIAL_NO_OVERLAP / LATENCY_SYNC | 双缓冲流水（TQue bufNum=2） | [double_buffer_design.md](double_buffer_design.md) |
| L1_VEC_BOUND（含 cast-heavy 反证） | 向量化效率 / 减 Cast | [vector_efficiency_design.md](vector_efficiency_design.md) |
| L1_MTE2_BOUND（真饱和） | dtype 收窄 / 数据量削减 | 机制卡 data-copy.md（族外共享） |
| L2_UNDERUTILIZATION | Block Dim 提升 | `ops-family/scripts/tiling_candidates.py --family elementwise`（候选生成 + 容量校验） |
| L2_HEAD_OVERHEAD（小 shape） | 减核 / 缩 TilingData | 机制卡 api-usage.md |

## Tiling 候选生成

```bash
python3 ops-family/scripts/tiling_candidates.py \
    --family elementwise --shape 8448,4096 --dtype fp16 --soc a5
```

多核 512 元素对齐 + UB 256B 对齐 + 每核 ≥4KB 下限已内置（候选表逐条附容量
占用计算）；fp16 add/sub 的 fp32 中间精度分支（use_fp32_cast）按算子属性开启。

## 模板货架（templates/）

[`templates/add_custom/`](templates/add_custom/)——可裁剪工程模板（源自
参考实现 验证样例）：双缓冲 TQue kernel + 多核均分/尾核
特化 + DataCopyPad 尾块 + host 直调 + torch 扩展双通路 + 验证脚本。
转换纪律见 [`templates/usage_guide.md`](templates/usage_guide.md)（只改
[MODIFY] 标记点，计时结构不许改）。

设计深度参考：[double_buffer_design.md](double_buffer_design.md)（双缓冲流水）/
[vector_efficiency_design.md](vector_efficiency_design.md)（向量化与减 Cast）。

## 知识缺口

发现本 guide 未覆盖的场景 → `ops-mechanism/scripts/log_gap.py --family elementwise
--kind guide_missing --note "<场景>"` 计数上报（季度排产输入）。
