---
type: OperatorRisk
title: SoftplusV2Grad / SoftplusBackward 算法约定
description: 围绕 Softplus 梯度角色与求值顺序定位，并按 repeat、极负区归零和属性残差直达案例支路。
tags: [SoftplusV2Grad, SoftplusBackward, 分支, repeat, subnormal]
---

# SoftplusV2Grad / SoftplusBackward 算法约定

当计算链包含 SoftplusV2Grad / SoftplusBackward 时，用本页补充梯度角色、分支和浮点求值顺序的敏感点。输出像某个中间量、阈值或极负区失配，以及大梯度下 Inf/有限类别不同，都属于相关线索。本页不声明任何产品上的 ACLNN API 可用，也不迁移 RegBase 历史案例的实现或修复。

## 按残余症状进入案例

已有部分修复时，直接按残差进入历史案例的对应支路；无需重新遍历全部候选。

| 残余症状 | 案例入口与首个检查 |
| --- | --- |
| actual 像 `self`、sigmoid 或其他中间量 | [支路 A：角色与槽位](../cases/softplus-v2-grad-family.md#支路-a数据流角色与槽位)，核对实参和 CopyOut 的最后写者 |
| 首 repeat 正常，后段重复或仅 tail 失败 | [支路 B：repeat、mask 与 tail](../cases/softplus-v2-grad-family.md#支路-brepeatmask-与-tail)，给真假支路设置可区分的合法值，比较跨 repeat 的具体 lane |
| 阈值两侧选支错误，或大梯度下 Inf/有限不同 | [支路 C：浮点语义](../cases/softplus-v2-grad-family.md#支路-c浮点语义而不是实数恒等式)，分开核对原始谓词、clamp 与参考乘除顺序 |
| 极负区为零，参考仍是小量；大梯度放大残差 | [支路 D：subnormal](../cases/softplus-v2-grad-family.md#支路-d设备精度与-subnormal)，在同一元素的产生、搬运、消费处定位首次归零 |
| 改属性后输出不变，或只在后续调用失配 | [支路 E：Host 属性与产物](../cases/softplus-v2-grad-family.md#支路-ehost-属性复用与产物采用)，核对真实属性、缓存与执行产物 |

案例中的 API、常量和环境仅覆盖正文范围；这里复用调查方法，不把历史实现当当前产品约定。

## 公式与角色

令 `bx = beta * self`。官方 SoftplusBackward 文档给出的反向公式是：

```text
gradInput = gradOutput * (1 / (1 + exp(-bx)))  if bx <= threshold
            gradOutput                         if bx > threshold
```

`gradOutput` 是上游梯度输入，`self` 是 Softplus 正向输入，`beta` 与 `threshold` 是定义公式的标量，`gradInput` 是输出。逐调用点把它们同实际形参、实参、dtype、地址/槽位及最后写者对应起来；同类型指针或寄存器位置相邻不代表角色可互换。

分支判据读取的是原始 `beta * self`：`bx > threshold` 直通 `gradOutput`，否则乘 sigmoid 因子。若为了计算 `exp` 使用稳定变换，仍保留 `bx_raw` 供谓词使用。把下界 clamp 后的量同时用于 false branch 和谓词，会改变分段语义；只有当前实现与独立 oracle 的 trigger/control 都支持时，它才是可证伪候选，不自动成为根因。

## 数学公式与参考实现的求值顺序

上面的 sigmoid 形式描述数学关系，不规定当前 reference 的浮点执行顺序。若 golden 调用某个框架 API，核对任务实际使用的版本、dtype 和分支；不能由 API 名称或这个数学公式推断它一定先计算 sigmoid 再乘梯度。

令 `z = exp(bx)`，分别展开 `(gradOutput * z) / (z + 1)` 和 `gradOutput * (z / (z + 1))`。标记中间 Mul/Div 的范围：大梯度可能让第一条链先溢出，而第二条链仍为有限数。reference 出 Inf、Kernel 出有限数时，先查实际求值顺序；“避免了溢出”本身不能证明符合当前验收要求。只有输入、分支和参考语义已确认，才能判断某种稳定改写是否允许。

用一组能让两条链分叉的合法输入和普通量级对照核对参考行为，保留中间乘积、比例及输出类别；已有可信观察可复用。通用区分方法见[乘除重结合](../mechanisms/algorithm-parameter-contract.md#乘除重结合的有限精度分叉)，历史诊断见[浮点语义支路](../cases/softplus-v2-grad-family.md#支路-c浮点语义而不是实数恒等式)。

## 最小定位检查

- 核对当前 golden 的实际执行链，把中间乘除、特殊值传播和后续放大标到计算链上；未确认顺序时先保留疑点，不用数学等价式替代。

- 选择 `bx` 位于阈值两侧、且两支预测输出不同的 control/trigger；先验证实际谓词使用的槽位和值，再比较 `gradInput`。两支恰好舍入为同一值时记录 `NO_DISCRIMINATION`，不能排除谓词错误。
- 对极负 `bx`，用当前任务冻结的 dtype 和逐步顺序计算 sigmoid 因子。若 actual 像 `gradOutput`、`sigmoid` 或另一个操作数，分别核对 `gradOutput/self/beta/threshold` 的实参槽位，而不是直接改公式或常量。
- 为每个候选值保留 `bx_raw`、稳定计算量、谓词输入、false-branch 结果和 `gradInput` 的最后 writer。CopyOut 必须从最后写入 `gradInput` 的槽位搬出；“VF 已写结果”不能证明搬出源正确。
- 比较 lower-clamp 开/关时，只改变 clamp，并保持槽位、谓词来源、dtype、CopyOut 和输入不变。只有输出按事前预测在 false branch 改变，才支持该候选；仍需证明 clamp 在当前约定中不应存在，才可归因。

## 使用范围

CANN 9.0.0 与 9.1.0 的 SoftplusBackward 产品约定均把 Ascend 950PR/950DT 标为该 ACLNN API 的“不支持”。本页可用于确认算法公式和参数角色；950PR 上的 API 支持、实现路径与数值模式必须另查当前版本的产品约定。
