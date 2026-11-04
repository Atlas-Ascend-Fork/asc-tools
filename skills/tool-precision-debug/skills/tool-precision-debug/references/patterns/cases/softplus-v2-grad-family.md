---
type: Case
title: SoftplusV2Grad：从错值形态选择调试支路
description: RegBase 历史排查示例，从错值形态选择参数、repeat、浮点语义、设备精度或 Host 属性调查支路。
tags: [SoftplusV2Grad, RegBase, 分支, 浮点语义]
---

# SoftplusV2Grad：从错值形态选择调试支路

历史排查示例：CANN 9.0.0 / Ascend950PR / DAV3510 / RegBase。下文按错值选择调查支路，具体原因需用当前输入确认。

从错值形态选择 repeat/mask、参数角色、CopyOut、浮点链或 Host 属性复用支路，再用当前输入与 oracle 设计区分实验。

## 什么时候使用

当前向量实现满足下列任一现象时打开本页：

- 小规模或首个 repeat 正确，跨 repeat、tail 或特定 lane 周期性失败；
- FP32 输出像 sigmoid 中间量，低精度输出却像 grad*bx 等另一条中间表达式；
- 常规值通过，极负区出现 0 对极小非零，或极值区出现 Inf 对有限值；
- 单次调用正确，同 shape 改变 beta、threshold 等属性后仍沿用上一次结果；
- 修完一个明显错误后，大范围随机或窄带输入仍有新的残差。

这些现象只负责选择支路。不要因为算子名相同，就直接复制公式改写、clamp、精度模式或常量。

## 先冻结真正的计算约定

在动 Kernel 前，从当前测试和 golden 中逐操作写出：

    bx = beta * self
    if bx > threshold:
        out = grad
    else:
        z = exp(bx)
        out = (grad * z) / (z + 1)

上式只是常见结构示意。必须重新确认当前任务的比较符号、二元求值顺序、dtype 提升、广播、NaN/Inf、signed zero 与容差。把以下信息保留在本次调试记录中：

- 能稳定复现的最小输入、seed、dtype、shape 和标量属性；
- 实际加载的 kernel/so 身份，避免把旧安装副本当作新构建；
- 第一个与 oracle 分叉的中间量，而不只记录最终误差；
- 每个候选的预测、单变量干预和否证条件。

## 第一步：用错值反解，不要先猜公式

为失败点计算一组候选值：

    bx
    z = exp(bx)
    z / (z + 1)
    grad * bx
    grad * z
    (grad * z) / (z + 1)
    grad * (z / (z + 1))
    grad

逐位比较 actual 与这些候选，保留 NaN/Inf 和 signed zero 类别。若 actual 稳定贴合某个中间值，优先追踪数据流角色和最后写者；若只在极值或 subnormal 区分叉，再进入浮点语义/设备精度支路。

| 观察 | 优先支路 | 暂不支持 |
|---|---|---|
| 错误按 repeat 或固定 lane 周期出现 | mask/repeat/tail 消费 | 全局公式错误 |
| actual 逐位等于 sigmoid 或 z | CopyOut 槽位、最后写者 | 随机精度噪声 |
| actual 数值接近 grad*bx，位模式另行比较 | VF 位置参数/逻辑角色错位 | 单纯 Exp 误差 |
| 仅极负区为 ±0，golden 为极小非零 | FTZ/DAZ 或中间下溢 | 地址整体错位 |
| Inf/有限类别随结合顺序改变 | oracle 求值顺序 | 用“数学等价”重写 |
| 同 shape A→B 结果仍像 A，换 shape 恢复 | Host executor/cache 身份 | Kernel 内公式单点错误 |

## 支路 A：数据流角色与槽位

画出一张表，不依赖变量名猜语义：

| 逻辑角色 | 物理 TBuf/LocalTensor | 首次写者 | 最后写者 | VF 形参位置 | 调用实参 | CopyOut 来源 |
|---|---|---|---|---|---|---|
| grad |  |  |  |  |  |  |
| bx |  |  |  |  |  |  |
| z/sigmoid |  |  |  |  |  |  |
| result |  |  |  |  |  |  |

重点检查两类“类型正确、角色错误”的问题：

1. VF 形参和实参都是同一种指针类型，编译器无法发现 bx 与 z、grad 等位置互换。
2. VF 已把最终结果写入结果槽位，但 FP32 或某条 dtype 路径从中间槽位 CopyOut。

单变量实验：只修一个角色映射或一个搬出来源，预测相关读取值或搬出值应如何改变。先确认新产物被采用、改动生效且观察能区分预测；有效观察与必要预测矛盾时，才排除被检验的具体绑定解释。最终错值不变时，检查目标中间值是否已恢复及是否有其他缺陷遮蔽，不能直接排除该绑定问题；参见[两个缺陷互相遮蔽](fused-rmsnorm-gelu-residual.md#两个缺陷互相遮蔽时怎么判断)。

## 支路 B：repeat、mask 与 tail

构造 count 位于一个 repeat 内、刚跨 repeat、多个 repeat、非对齐 tail 的四组输入，并给真假分支填入容易识别的哨兵值。

- 只在跨 repeat 后失败：检查 Select/比较 API 的 mask 模式是否覆盖每个 repeat，mask 是否被截断或复用。
- 只在 tail 失败：检查有效元素数、repeat 次数、末 repeat mask 与搬出长度是否使用同一逻辑长度。
- 首个 repeat 也失败：回到角色/公式支路，不要用扩大 mask 掩盖全域错误。

干预前先预测具体哪些 lane 会改变；“误差数量下降”不足以证明 mask 根因。

## 支路 C：浮点语义而不是实数恒等式

分别比较这些执行链的位级结果：

    (grad * z) / (z + 1)
    grad * (z / (z + 1))

它们在实数域等价，在浮点极值、Inf 和舍入边界上不等价。只有 golden 的逐操作约定允许时才能改写。

同样地：

- 不符合算法定义的下界 clamp 会把极负 bx 映射到错误的非零区；
- 若先改写或钳位 bx，再用它判断 bx > threshold，会同时改变分支谓词；
- 为“数值稳定”改公式前，必须覆盖 NaN、Inf、signed zero、极大 grad 和阈值两侧。

判定标准不是平均误差更小，而是逐操作语义和特殊值类别与 oracle 一致。

### 公开回归通过后仍漏掉求值顺序

另一次 FP32 直调诊断中，候选修复了掩码和极负输入问题，公开回归全部通过，却把 reference 解释为“先计算 sigmoid 再乘 grad”，保留了先除后乘。独立检查实际使用的 PyTorch 2.7.1 CPU `aten.softplus_backward`，确认该分支先乘后除。

以 FP32 的 `grad=3.2e38、self=2、beta=1、threshold=20` 调用该 reference，输出为 `+Inf`；CPU 上先除后乘得到约 `2.82e38`。二者的中间乘积范围不同，“结果有限”和“公开回归通过”均不能证明保持了该参考行为。普通量级样本不足以区分这两条链，应另选能暴露范围差异的合法输入。

这个观察补充的是参考求值顺序的核对方法。上述具体数值来自同版本 CPU 探针，不是候选 NPU 输出的逐元素重放；尚未完成更改候选后的 NPU 回归，不作为已验证修复方案，也不外推所有 Softplus 实现都必须采用这个顺序。

## 支路 D：设备精度与 subnormal

当残差集中在 0 对极小非零或一个很窄的输入带时，把“产生、计算、搬运”拆开探测：

1. 直接生成 subnormal 并 CopyOut，判断搬运是否保留；
2. 分别只执行 Exp、Mul、Div，定位第一次变为零的位置；
3. 对比不同受支持精度或非 FTZ 模式，确认模式实际作用于哪个指令；
4. 保存一个确定性窄带复现器，按位检查 oracle 和设备输出。

Exp 接近 1 的微小差异可能被巨大 grad 放大成 Inf/有限分叉。先证明误差在哪个原语产生，再决定是否存在符合约定的精度模式；不要从某个输入点反推 magic constant 或局部补丁。

## 支路 E：Host 属性复用与产物采用

用相同 shape 连续执行：

    A(beta/threshold = a) → B(beta/threshold = b)
    B → A
    A → 改 shape 的 B

若第二次调用跟随第一次属性，而改 shape 后恢复，检查 executor/cache key 是否包含所有影响结果的标量属性，以及 tiling/kernel 参数是否随调用刷新。

每轮 Kernel 实验还要核对构建产物时间/哈希、安装目标和运行时加载路径。源码已改但 actual 完全不动，先排除旧副本，不要继续堆叠修复。

## 修复后的回归矩阵

至少覆盖：

- 首 repeat、跨 repeat、多 repeat、非对齐 tail；
- FP16/BF16/FP32 等实际支持 dtype 与广播组合；
- bx 在阈值两侧、极负区、零附近和大正区；
- 正常、极大、零、Inf/NaN grad，以及 signed zero；
- subnormal 产生/计算/搬运探针和保存的窄带复现；
- 同 shape 不同属性的 A→B、B→A 调用顺序；
- 原失败输入、固定小集、随机集和连续重复运行。

每修一项都重新对残差分群。只有原失败闭合、每条已采纳根因有独立证据、回归矩阵通过且实际产物身份明确，才能结束该线程。
