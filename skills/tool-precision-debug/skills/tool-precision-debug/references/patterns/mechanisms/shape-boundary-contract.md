---
type: Mechanism
title: Shape、Rank 与边界维度约定
description: 分别核对逻辑 Shape、Rank、空维与边界映射，区分 shape 约定、broadcast 和 tiling 切分问题。
tags: [Shape, Rank, 空维, 边界]
---

# Shape、Rank 与边界维度约定

当空维、维度为 1、最小或极大 shape、reshape/squeeze 后的 rank 变化触发异常时，先验证 shape 约定，不要直接归因于 tail。Shape 约定描述逻辑维度是否合法以及如何映射；tiling 描述合法 shape 如何切分，broadcast 描述两个逻辑 shape 如何对齐，三者必须分开。

## 建立 shape 账本

同时记录 Host 推导值和 Kernel 实收值：原始 rank、每个维度、axis 规范化结果、输入/输出元素总数、空张量策略、维度 1 的保留或消除、动态区间以及选中的实现分支。对 reshape/flatten/squeeze 写出变换前后的坐标等式，不能只比较元素总数。

## 支持与反证

- 只在 rank、空维或维度 1 边界突变，并且 Host 与 Kernel 对 shape 的解释不同，支持 shape 候选。
- 合法 shape 已一致传入，但错误从最后一个不完整 tile 开始，转查 [Tiling、尾块与跨 tile 状态](tiling-tail-state.md)。
- 错误沿广播轴周期重复，转查 [广播与转置算子族](../operators/broadcast-transpose.md)；沿物理 stride 重复则转查搬运布局。
- 相邻 shape 选择了不同 tiling key 或 kernel 符号，先检查动态分派，而不是修改通用 shape 公式。

## 区分实验

构造维度为 0、1、边界前一值、边界值和边界后一值的最小矩阵；每次只改变一个维度，保持 dtype、layout、核数和其他属性不变。保存 Host shape 推导、tiling 数据和 Kernel 入口值。若需要强制分支，预先写明每个分支的预测，并确认实际二进制采用了强制结果。

### Rank-0、空维与维度 1 分账

- **何时查**：标量、含 0 的维度、维度 1、squeeze/unsqueeze 或负 axis 规范化后结果突变时查询。
- **查什么**：分别记录 rank、完整维度向量、逻辑元素数、axis 规范化前后值和输出 shape；rank-0、`shape=[1]` 与含空维 Tensor 在约定裁决前必须作为不同状态，不能因元素数相同或都“很小”而合并。
- **怎么证伪**：按公开约定构造最小合法邻域，并对每个输入检查 Host 推导与 Kernel 实收完全一致；如果错误只跟最后一个 tile 的有效长度移动而不跟 rank/axis 状态移动，就降低 shape 候选。
- **避免混淆**：空输出无需计算不等于可以跳过当前约定对 Host 校验、workspace 要求和输出 shape 的裁决；维度 1 上的周期复用转广播，合法 shape 的非整尾块转 tiling/tail，运行时选错实现转动态分派。

### 等价 shape 的变形关系

- **何时查**：reshape/squeeze 后才失败，或两个元素总数相同的 shape 走出不同结果时查询。
- **查什么**：只在算子约定明确允许时，建立插入/删除维度 1、reshape 后逆 reshape、axis 随 rank 同步平移等变形关系；为每条关系写出输入坐标、输出坐标及预期不变量，不只写 volume 相等。
- **怎么证伪**：用同一坐标编码数据跑原 shape 与变形 shape，并把结果映回同一逻辑坐标；若差异随物理 layout/tiling 改变而变形前后的 Host/Kernel shape 约定均一致，则降低纯 shape 候选。
- **避免混淆**：变形通常会同时改变 kernel、tiling、连续性或多核分区；这些变化必须作为混杂单列。一次等价变形通过也不证明其它 rank、axis 或动态区间正确。

### 动态区间的等号审计

当自然问题是“511 正常、512 或 513 失败”时，先确认 512 是否真是当前源码或配置中的边界，而不是因为数字接近常见 tile 大小就假设它是边界。设真实边界为 `B`，逐项核对：

1. Host 对 `B-1`、`B`、`B+1` 的合法性、rank/axis 规范化和输出 shape 推导。
2. 每个相邻区间的谓词是否无空洞且无重叠，尤其是 `< B`、`<= B`、`== B` 和默认分支。
3. Host 选择的分支、tiling 字段与 Kernel 实收字段是否使用同一边界语义。
4. 三个输入的逻辑元素集合是否只改变目标维度；若核数、layout 或属性也变化，将其单独列为混杂。
5. 每条分支分别处理整块、余 1 和最大合法 tail 时，输出 shape 与有效区是否仍符合公开约定。

只有 Host 与 Kernel 在等号处采用不同区间、某个合法 shape 落入空洞/重叠，或强制正确分支后首偏离边界按预测移动，才支持动态 shape 约定候选。若 shape 已被双方一致解释而错误只从最后一个不完整 tile 开始，应转向 tiling/tail；若源坐标在广播轴上已经错误，则转向广播语义。

相邻 shape 选择不同 kernel 是观察，不证明任何一条分支错误；511/512/513 全部通过也只覆盖该边界邻域，不证明整个动态区间。诊断性强制分支可能同时改变资源和调度，不能直接成为正式修复。

## 修复与回归

在 Host 和 Kernel 之间只保留一个明确的 shape 约定，显式拒绝非法输入；不要用 `max(1, dim)` 或固定最小 tile 静默改变算子语义。回归覆盖 rank、空维、维度 1、动态区间两侧以及与 broadcast、tail 组合的合法输入。
