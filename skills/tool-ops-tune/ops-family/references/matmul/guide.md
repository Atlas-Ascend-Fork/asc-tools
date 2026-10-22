# MatMul 族性能优化指南（六策略互斥判定树）

> A 型（单实现调参）。适用 DAV_3510。策略间适用条件构成**互斥判定树**——按证据特征
> 选策略，禁止「全都加上」。
>
> 各策略的完整设计文档（优化目标量化收益 / 架构概览 / 关键参数 / 核心循环前后对照 /
> 关键修改点）见本目录下 `*_design.md` 与 [best-practices-guide.md](best-practices-guide.md)。

## 六策略判定树

```
判定输入（verdict findings + shape/tiling 特征）
│
├─ 所有 matmul 基线 ────────────────→ ① pingpong（L1/L0 双缓冲 + MTE2/MTE1/Fixpipe
│                                      事件同步流水化；其它优化的默认前置）
├─ MN 够并行（totalBlockCnt > aicNum 数倍）且尾碎片负载不均
│                                   → ② swat（蛇形行窗口 + 尾块合并 + 末轮二维再切分）
├─ MN 欠并行（totalBlockCnt < aicNum）+ K ≥2048
│                                   → ③ streamk（SK / DP+SK：切 K 到空闲核，AIV 归约）
├─ 小侧矩阵 ≤ L1/2 + 对侧 T≥2 + 真 MTE2 bound
│                                   → ④ fullload（小侧 + 随路 Scale 一次性驻留 L1）
├─ 假 MTE2 bound（busy 高但带宽利用率 <70%）+ <20KB 小块
│                                   → ⑤ scale_coalescing（scaleKL1 = SCALE_L1_BUFFER_NUM
│                                      × kL1 合并成一次大 MTE2）
└─ pingpong 已开 + 各流水 busy ≤70%（准无 bound）+ 流水图 PING/PONG 间有 gap
                                    → ⑥ mte2_preload（三段结构：首轮 PING/预取 PONG/消费；
                                       零 TilingData 数值改动）
```

## 策略-判定规则映射

| 策略 | 触发证据（verdict） | 设计文档 |
|------|--------------------|---------|
| ① pingpong | 基线必选（L1_SERIAL_NO_OVERLAP 命中即受益） | [pingpong_design.md](pingpong_design.md) |
| ② swat | L2_CORE_TAIL / L2_IMBALANCE_*（尾碎片形态） | [swat_design.md](swat_design.md) |
| ③ streamk | L2_UNDERUTILIZATION（MN 欠并行） | [streamk_design.md](streamk_design.md) |
| ④ fullload | L1_MTE2_BOUND + 反证 true-bandwidth-saturation（真饱和） | [fullload_design.md](fullload_design.md) |
| ⑤ scale_coalescing | L1_MTE2_BOUND + 反证 fake-mte2-small-packet（假 bound） | [scale_coalescing_design.md](scale_coalescing_design.md) |
| ⑥ mte2_preload | 全 pipe busy ≤70%（准无 bound）+ 流水图 gap | [mte2_preload_design.md](mte2_preload_design.md) |

## 容量约束公式（fullload/streamk 前置核算）

```
L1 预算: 分形 A + 分形 B + ping-pong 缓冲 ≤ L1 总容量
L0 预算: mL0 + nL0 + kL0 ≤ L0A/L0B/L0C 分域容量
UB 预算: tile 输入 × bufNum(2) + 输出 ≤ UB 容量
```

## 叠加关系

① 为其它策略前置；②③ 与 ④⑤ 通常互斥（并行度形态相反）；⑥ 可与 ① 叠加；
⑤ 消除后复查是否转化为 ④ 形态。
