# FA 族设计：三通道 Roofline（基本块 Bq/Bk 选取）

> **来源（蒸馏）**：Ascend C Tiling 设计方法论·FlashAttention Roofline 分册。
> 本文是 FA 基本块选取的**推导方法论**——禁止凭经验选取 Bq/Bk。

## 1 为什么 FA 需要专门的 Roofline 模型

FA 是 mix kernel（AIC 算 GEMM + AIV 算 softmax，跨核 stage 化），性能瓶颈
**不能用单一 AI 概括**。三个独立吞吐通道各自可能成为瓶颈：

1. **Cube 算力**（AIC 的 Mmad 吞吐）
2. **HBM 带宽**（K/V/Q/O 的 GM 加载与写回）
3. **跨核/片上吞吐**（AIV softmax 吞吐 + Cube↔Vector 片上握手 + O_acc streaming）

> ⚠️ **常见建模错误**：把片上跨核握手（S/P/PV 的 L0C↔UB↔L1 movement）与 HBM
> 加载混进同一个 AI 分母，再用「混合带宽」求单一 AI。这会把 AI 人为封顶在
> `D/sizeof(T)`，让人误以为 FA 永远到不了 compute-bound，同时掩盖真正的第三
> 瓶颈（AIV/片上吞吐——mix kernel 最常见瓶颈）。**三通道各自独立建模，
> 性能 = min(三 ceiling)，瓶颈 = 限制项。**

## 2 GQA 多头合并：mEff

FA 类 Mmad 的有效 m 维不是 Bq 本身：

```
mEff = Bq × curG
```

curG = 同一 kvHead 下合并到一个 task 的 qHead 数（≤ G = Hq/Hkv）。

- **Prefill**（Sq 大）：Bq 够大，mEff 主要受 L0A 容量约束，curG 可能 < G
- **Decode**（Sq=1）：Bq=1，`mEff = curG`，应最大化 curG = G

**mEff 是 FA Roofline 的核心自变量**——同时决定 AI 与三通道相对松紧。
（GQA 合并本身是不变量 I2：同 kvHead 的 G 个 qHead 必须同 task，禁止拆 G 个
task 各自重复搬 KV——KV 重复搬运 G 次，MTE2/L1 利用率塌陷。）

## 3 三通道模型

### 3.1 通道 2：HBM（主 Roofline）

单 task 遍历全 Sk 的流量与算量：

```
FLOPs_total = 4 × mEff × Sk × D          # C1 与 C2 两个 GEMM
Bytes_HBM ≈ 2 × Sk × D × sizeof(T)       # K/V 加载（主导，Sk ≫ mEff 时）
             + 2 × mEff × D × sizeof(acc) × (Sk/Bk)   # O_acc streaming（仅 streaming 模式）

AI_HBM ≈ 2·mEff / sizeof(T)              # 忽略 streaming 项的清晰上界
ridge_HBM = Peak_CUBE_FLOPS / Peak_HBM_BW
AI_HBM < ridge_HBM → HBM-bound 可能
```

（峰值参数从 soc-spec.yaml 取，不硬编码。）

**关键结论**：
- **AI_HBM ∝ mEff**（不封顶在 D/sizeof(T)）；
- **Decode（mEff=G）的 AI ∝ G**——GQA group 合并是提升 decode AI 的唯一杠杆；
- **Bk 经 O_acc streaming 项影响 AI**：streaming 模式增大 Bk 减少 O_acc 往返
  （Sk/Bk 次）；非 streaming（O_acc 驻 UB）Bk 对 AI 无直接影响。

**causal 的影响不在 AI**：完全 mask 的 tile 被跳过，FLOPs 与 K/V 加载**同比例
减少**（平均有效 KV ≈ Sk/2），Sk 在 AI 推导中约掉 → AI ≈ 不变。causal 真正
影响**绝对耗时与核间负载均衡**（见 mixed_kernel 文档 §多核切分）。

### 3.2 通道 3：跨核/片上吞吐（AIV-bound 校核）

若 AIV 端吞吐 < AIC 端，即使 HBM 与 Cube 都不饱和，算子仍被 AIV 拖住。

```
T_AIC(tile) ≈ FLOPs(tile) / (Peak_CUBE × cube_util)
T_AIV(tile) ≈ softmax_ops(mEff·Bk) / AIV_vector_throughput
              + onchip_bytes(mEff·Bk + mEff·D) / Peak_onchip_BW

cross-core-bound ⟺ T_AIV > T_AIC
```

AIV 端每 tile 工作量：softmax over `[mEff, Bk]`（ReduceMax/Exp/ReduceSum ∝
mEff·Bk）+ 片上握手 movement（S: L0C→UB `mEff×Bk`；P: UB→L1 `mEff×Bk`；
PV: L0C→UB `mEff×D`——不占 HBM，占片上 SRAM 带宽）。

**主杠杆**：提 AIC:AIV 比（`__mix__(1,2)` → 更多 AIV）；减握手粒度、
per-stage 同步、双缓冲让 AIC/AIV 重叠；减 PipeBarrier 冗余（<30%）。

### 3.3 通道 1：Cube 算力

```
Perf_compute = Peak_CUBE_FLOPS × cube_util
```

cube_util 主要由 Mmad m/n/k 对齐到 16 的倍数决定。

### 3.4 三通道取 min

| 区制 | 判据 | 主杠杆 |
|------|------|--------|
| Compute-bound | AI_HBM ≥ ridge 且 AIC 吞吐最低 | Mmad 对齐、Cube/Vector 重叠、减 PipeBarrier |
| HBM-bound | AI_HBM < ridge 且 HBM 吞吐最低 | 增大 mEff（∝AI）、Q 驻 L1、增大 Bk 减 O_acc 往返 |
| Cross-core-bound | T_AIV > T_AIC | 提 AIC:AIV 比、减握手粒度、双缓冲、减 PipeBarrier |

常见 D 档位、mEff 较小时通常先撞 **HBM-bound 或 cross-core-bound**；增大 mEff
提升 AI 后可能转入 compute-bound 或 cross-core-bound。

## 4 基本块选取要点（推导，不照抄）

- **K_BASE 尽量取到 D**（L0A/L0B 容量允许时）：`K_BASE = D` 单次 k-pass 完成
  Mmad；K_BASE 越小 k-pass 次数越多，每次带 SetFlag/WaitFlag/PipeBarrier 同步
  开销——**选 K_BASE 必须代入 k-pass 次数评估同步开销**；
- **M_BASE / N 轴步进**：L1/L0 单槽容量约束下取尽量大（减分块循环次数），
  上界由容量校验公式给出（运行时 `GetCoreMemSize` 查询，禁硬编码）；
- 初始试验值：`K_BASE=D`、M/N 步进取容量允许的最大 16 倍数，按实测调整；
- **Bk**：streaming UB 模式下优先增大（减 O_acc 往返）；受 softmax 状态 buffer
  与握手 slot 容量约束。

## 5 量化 FA 的 AI 修正（MX 类）

MX 类（mxfp8/mxfp4）块量化格式 scale 必须沿被消费 matmul 的 **reduction
（K-mmad）轴**量化（硬件硬约束："data must be consecutive over the reduction
dimension"）：

| 张量 | 被消费 matmul | K-mmad 轴 | scale 量化轴 |
|------|-------------|----------|------------|
| Q | Q·Kᵀ | D | D |
| K | Q·Kᵀ | D | D |
| P（kernel 内构造） | P·V | S_k | S_k |
| V | P·V | S_k | **S_k（不是 BSND innermost 的 D！）** |

**反直觉陷阱**：V 在 BSND `[B, Sk, Hkv, D]` 下 D 是 innermost 连续维度，按
「沿连续维度量化」直觉会得 V scale 沿 D——与 PV Mmad 的 K-mmad=S_k 不兼容，
业界标准沿 S_k。**违反后果：Mmad ScaleB 轴错位 → 输出 NaN/inf**（直接非数，
不是漂移）。

## 6 与本仓工具链的联动

- 三通道的 Peak 参数：`ops-verdict/references/soc-spec.yaml`（逐字段溯源）；
- 上板后区制验证：verdict 命中规则 ↔ 区制对照表见 [guide.md](guide.md)；
  `mte2_active_bw_pct_of_peak`（facts_bridge 产出）直接服务 HBM 通道判定；
  overlap_pct（trace_bubble 产出，需 A5 PipeTimeline/仿真）服务 cross-core
  通道判定；
- 候选参数探索：容量约束消费 soc-spec（与 tiling_candidates.py 同款容量
  预算思路，FA 专用基本块推导按本文 §4）。
