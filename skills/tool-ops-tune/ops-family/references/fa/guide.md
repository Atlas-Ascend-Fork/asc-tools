# FA 族指南（guide）——FlashAttention 类算子调优入口

> **来源（蒸馏）**：内容整理自 Ascend C Tiling 设计方法论·FlashAttention 分册
> 四层方法论（patterns / foundation / design / subfamilies，约 2000 行）+
> 业界 Attention 设计实践（CATLASS 路径与标杆
> 对齐）——P1-7① 台账残留项的先期知识就位（真实 FA 工程接入时方法论文档已可用，
> 实战案例仍按无同型降级策略沉淀）。

## 族特点

FA（FlashAttention）类 = 分块化、不显式实例化 attention score 矩阵的 attention
计算族：Q/K/V 三输入，计算图 **Q·Kᵀ → softmax → ·V** 两段 GEMM + 中间 online
softmax（FA-2），沿 KV 维度分块流式累积，不在显存构建完整 `[Sq, Sk]` 矩阵。

**与其它族的本质差异**（决定不能套用 reduction/elementwise/matmul 模式）：

| 维度 | FA 类 | 其它族 |
|------|------|--------|
| Kernel 类型 | `__mix__(N, M)` AIC+AIV 协同（典型 `__mix__(1,2)`） | 多数纯 vector / 纯 cube |
| 计算流 | 跨核 stage 化（Cube 算 GEMM，Vector 算 softmax） | 单核内完成 |
| 跨 KV 分块状态 | 必须在线 softmax 累积（max/sum/O_acc） | 多数无在线状态 |
| 任务维度 | s2（KV 分块）**禁入**任务分发维度 | 多数所有维度可分发 |

**禁止**：`__vector__`（AIV_ONLY）做 FA 类——纯 Vector 实现 GEMM 性能比 Cube 低
10-100×；仅 MVP 调试阶段可临时做精度基线。

## 判定规则映射（verdict 命中 → 本族机制）

FA 是 mix kernel，瓶颈形态跨 L1/L2/L3 三层，本仓 verdict 的消费方式：

| verdict 命中 | FA 场景解读 | 深入文档 |
|-------------|------------|---------|
| L1_MTE2_BOUND（真饱和） | K/V 加载带宽主导（HBM-bound 区制） | [fa_roofline_design.md](fa_roofline_design.md)（增大 mEff 提升 AI） |
| L1_LATENCY_SYNC_SUSPECT / 串行指纹 | AIC/AIV 握手未流水化（cross-core-bound 前兆） | [fa_mixed_kernel_design.md](fa_mixed_kernel_design.md)（流水编排） |
| L1_VEC_BOUND | AIV softmax 吞吐主导（cross-core-bound） | 同上（提 AIC:AIV 比 / 减握手粒度） |
| L1_CUBE_BOUND | Mmad 对齐/利用率问题（compute-bound） | fa_roofline（Mmad 对齐杠杆） |
| L2_UNDERUTILIZATION / CORE_TAIL | causal 负载不均 / task 维度构造问题 | fa_mixed_kernel（多核切分与均衡） |

> 注意：L3 MC² 规则不适用于单卡 FA；多卡 FA（sequence parallel）按 MC² 场景
> 单独判定。**片上跨核握手不是 MTE2 带宽**——roofline 三通道模型见专文，
> 禁止把握手流量混进 HBM AI 分母。

## 调优路由（按性能区制）

三通道 Roofline 判定（[fa_roofline_design.md](fa_roofline_design.md)）出区制后：

| 区制 | 主杠杆 |
|------|--------|
| HBM-bound | 增大 mEff（∝AI）；Q 驻 L1；增大 Bk 减少 O_acc streaming 往返 |
| Compute-bound | Mmad m/n/k 对齐 16；Cube/Vector 重叠；减 PipeBarrier |
| Cross-core-bound | 提 AIC:AIV 比（如 `__mix__(1,2)`→更多 AIV）；per-stage 同步；双缓冲重叠；减 PipeBarrier |

## 标杆对齐（对照业界 Attention 设计实践）

| 标杆 | 位置 | 用途 |
|------|------|------|
| `aclnnFlashAttentionScore` | CANN op_api 文档 | 底层标杆：语义/dtype/layout/scale/mask 权威 |
| `torch_npu.npu_fusion_attention` | torch_npu op-plugin | Python 标杆：可直接 NPU 实测对比 |

关键映射陷阱（蒸馏）：
- **seqlen 约定不同**：npu 标杆 `actual_seq_qlen/kvlen` 是**累加和**，内核常为
  per-batch 原始长度——host 必须做差分转换；
- **mask 语义**：`atten_mask 1=不参与`；causal（sparse_mode 2/3）须压缩下三角
  `[2048,2048]` 否则 tiling 报 161001；
- **scale**：默认 `1/√D`；标杆传自定义 scale 时 host 须覆盖 TilingData；
- dropout/pse/sink/inner_precise 等字段不在前向调优范围，静默继承 = 语义错误。

## 文档结构

| 文档 | 内容 |
|------|------|
| 本 guide | 族定位 / 判定映射 / 调优路由 / 标杆对齐 |
| [fa_roofline_design.md](fa_roofline_design.md) | 三通道 Roofline（mEff/AI 推导 / 区制判定 / causal 与量化修正） |
| [fa_mixed_kernel_design.md](fa_mixed_kernel_design.md) | mix kernel 设计（数据流四阶段 / 不变量 I1-I5 / 流水编排 / 跨核同步 / softmax 实现陷阱） |

## 模板状态（如实标注）

本族**无可拷贝代码模板**（模板数 0）——Ascend C 路径无完整 FA 参考实现
（asc-devkit 仅有 mix 融合示例 + SoftmaxFlashV2 building block 可作基线验证）；
CATLASS 路径的整经验证核函数（`FAInferKernel`）在 参考实现 工程资产内，
不在本仓。实施时按 [fa_mixed_kernel_design.md](fa_mixed_kernel_design.md) 的
设计节点推进 + 无同型降级策略（验证强度↑）。

## 知识缺口

实战案例（E 级）待首个真实 FA 工程接入后沉淀：发现本 guide 未覆盖场景 →
`ops-mechanism/scripts/log_gap.py --family fa --kind family_missing --note "<场景>"`。
