---
type: Case
title: FusedRmsNormGeluResidual：用退化恒等式拆融合流水
description: 历史排查示例，用退化恒等式和参数哨兵拆解融合流水，区分角色绑定、缓冲生命周期与跨 tile 状态。
tags: [RMSNorm, GELU, 融合算子, Buffer]
---

# FusedRmsNormGeluResidual：用退化恒等式拆融合流水

历史排查示例：CANN 9.0.0 / Ascend910B1 / DAV2201 / kernel-direct。共享 FP16 临时区覆盖仍是待验证支路，不能直接当作当前问题的根因。

用退化恒等式与参数哨兵拆开融合流水，分别检查角色绑定、共享临时缓冲的生命周期和跨 tile 状态。每条支路都需要当前输入下的区分实验。

## 什么时候使用

多输入融合算子同时包含归一化、激活、门控、仿射和 residual 时，最终全域失配通常不足以指向某一级。出现以下指纹时优先使用本页：

- 修改 gamma、scale 等参数，输出几乎不变，修改另一个参数却异常主导结果；
- CPU/NumPy 逐级计算正确，但真实 Kernel 从某个 LocalTensor 或流水阶段开始分叉；
- 参数已 preload，消费端却可能读取同类型 scratch；
- 多个参数经过同一个临时缓冲转换，最终值像“最后一次加载的参数”；
- 修正一个明显绑定后几乎没有改善，怀疑第二个缺陷仍在遮蔽结果。

目标不是一次猜中根因，而是把融合链拆成能单独证伪的参数敏感性、角色绑定和生命周期实验。

## 先冻结阶段约定

把当前实现拆成有明确输入/输出的阶段，例如：

    x
     → LayerNorm
     → RMSNorm(gamma)
     → GELU
     → Gate(beta, gate)
     → Affine(scale, bias)
     → ResidualAdd(residual)
     → y

阶段名称和顺序必须以当前 oracle 为准。为每一级记录逻辑角色、dtype、shape、物理槽位、producer、consumer 和 CopyOut 点。不要因为最终输出是 FP16，就忽略 FP32 中间量的第一次分叉。

先确认实际 shape 是否只有一个 tile/row。只有在多 tile 或同核多 row 时才会执行的状态分支，不应在单 tile 复现中被列为首要根因。

## 第一步：用退化恒等式定位阶段

为参数选择能让某些阶段失效或退化的值，并在改代码前写出预期输出：

| 探针 | 预期用途 |
|---|---|
| gate = 0 或 scale = 0 | 让融合激活贡献消失，结果应退化为 residual + bias |
| gamma = 0 或能使归一化输出退化的参数 | 隔离归一化到激活链与末端仿射/residual |
| 常量 x | 让归一化结果更容易手算，暴露均值/方差与参数读取问题 |
| residual = 0 | 单独观察融合主链和 bias |
| bias = 0 | 区分 bias 读取错误与 residual 错位 |
| 每个参数使用不同哨兵模式 | 识别消费端实际读到了哪个参数或 scratch |

退化结果不满足时，从最后一级向前检查；满足时，说明被消去的前级暂时不是该探针下的首个分叉。每个探针只回答一个问题，不把“看起来更接近”当作通过。

## 第二步：建立参数敏感性矩阵

保持其他输入不变，分别只扰动 gamma、beta、gate、scale、bias 和 residual，记录：

- oracle 哪些输出元素应改变、方向和量级；
- actual 哪些元素实际改变；
- actual 是否更像另一个参数被扰动；
- 哪个中间 checkpoint 首次失去敏感性。

若 gamma、beta、scale 等多个参数的中间值都等于最后加载的 bias，优先进入共享临时缓冲生命周期支路；若 preload 目标正确但消费端对某参数完全不敏感，优先进入角色绑定支路。

## 支路 A：逻辑角色绑定到错误物理槽位

建立下表并从 producer 追到 consumer，不依赖相似变量名：

| 逻辑参数 | GM 来源 | preload 最终槽位 | 消费端 LocalTensor | 消费阶段 | 最后写者 |
|---|---|---|---|---|---|
| gamma |  |  |  |  |  |
| beta |  |  |  |  |  |
| scale |  |  |  |  |  |
| bias |  |  |  |  |  |
| residual |  |  |  |  |  |

常见失效是专用参数槽位已经正确写入，消费阶段却从 residual 或通用 scratch 取得同 dtype、同 shape 的 LocalTensor。编译和地址合法性检查可能都通过。

单变量实验：

1. 只把一个消费角色接回其 producer 最终槽位；
2. 使用该参数的独立哨兵输入；
3. 预测只有依赖该参数的阶段和输出会改变；
4. 若 actual 仍等于旧 scratch 的候选值，检查产物采用或否定该绑定。

不要同时清零所有 buffer、添加全局同步或改核数，否则无法知道是哪条角色边生效。

## 支路 B：共享临时缓冲的生命周期与流水覆盖

当多个参数采用类似流程：

    GM → shared FP16 temp → Cast → dedicated FP32 parameter

仅看到源码顺序或 barrier 名称，不能证明前一次 Cast 消费完毕后，下一次 MTE2 才覆盖 shared temp。画出真实生命周期：

    MTE2 write temp
      → ownership transfer
      → V read temp / Cast
      → V completion
      → temp 可复用

重点检查：

- 同一个 TBuf 是否被连续用于多个 DataCopyPad/Load；
- producer/consumer 是否通过正确的 queue 所有权转移；
- Alloc、Load、EnQue、DeQue、Cast、Free 或等价机制是否包住每一次临时数据生命周期；
- 局部 barrier 是否只约束某条流水，而没有阻止另一条流水提前覆盖；
- 每个 FP32 参数槽位的值是否实际都来自最后一次加载。

区分实验可选其一：

- 为每个参数临时使用独立且可辨识的 FP16 槽位；
- 将一次传输改成明确的 Alloc → Load → EnQue → DeQue → Cast → Free 生命周期；
- 在最小复现中只保留两个参数加载，交换加载顺序并预测“最后加载者”是否随之改变。

如果交换顺序后被错误复制的参数也随之改变，强力支持覆盖假设；若不变，回到绑定或 producer 数据本身。

## 两个缺陷互相遮蔽时怎么判断

绑定错误与临时缓冲覆盖可以同时存在：

- 只修生命周期，消费端仍从错误 scratch 读取，最终输出可能毫无改善；
- 只修消费绑定，专用参数槽位本身仍被最后一次加载覆盖，也可能只改善少量样本；
- 两项一起修复后通过，并不能自动证明每一项的因果性。

因此在正式修复前保存参数 checkpoint，并为两项干预分别预测中间值：

| 干预 | 应先恢复的观察 |
|---|---|
| 只修消费绑定 | consumer 改为读取专用槽位，即使槽位内容仍错误 |
| 只修生命周期 | 各专用参数槽位恢复各自哨兵，即使最终 consumer 仍错 |
| 两者都修 | 参数槽位、阶段输出和最终输出同时闭合 |

中间 checkpoint 能把“最终误差没下降”的遮蔽关系拆开。

## 先证明候选在当前 shape 上可触发

看到可疑代码不等于它解释当前失败：

- tile > 0、跨 row 残留等分支只有在当前 tiling 真正产生多个 tile/row 时才可作为 active root；
- 单核运行只排除部分跨核 ownership 问题，不能排除同核多 row 的状态复用；
- 添加某个 V→S 或全局 barrier 后结果不变，应降低该同步候选优先级；
- UB 容量估算、编译成功和静态检查无 finding，都不能替代“错误值来自哪个 writer”的证明。

每条候选必须同时满足触发条件、错值预测和单变量干预三项。

## 修复后的回归矩阵

至少覆盖：

- 所有退化恒等式与独立参数哨兵；
- 每个参数的正负、零、常量、随机和边界取值；
- 单 row/多 row、单 tile/多 tile、单核/多核和尾块；
- 各阶段 FP32 checkpoint 与最终 dtype 输出；
- 原失败输入、随机输入、连续重复运行；
- 构建产物与运行时加载身份。

结束条件是：首个分叉已定位到具体 producer-consumer 或生命周期边；每项修复有独立可否证证据；组合修复不再依赖掩蔽；原失败和回归矩阵均通过。
