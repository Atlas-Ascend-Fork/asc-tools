---
name: ops-family
description: 算子族实践库与模板代码货架。按算子族（matmul/mc2/reduce/elementwise/broadcast/conversion/sort/scalar/simt…）组织的实现范式、A/B 双型文档与可拷贝代码模板（.h/.cpp/.template）。实施环节的模板优先纪律（直接拷贝 + 最小适配 + 编译修复顺序）在本 SKILL 落地。触发：查算子族优化参考实现、查模板代码、实施方案落地时使用。
---

# 算子族实践库（family）

> 职责：机制确定后「怎么写」的货架。查到模板**必须直接拷贝**，禁止参考重写——这是
> 知识层与编排层的双重契约（verdict→mechanism 路由到此，模板路径进候选方案卡）。

## 算子族分类表

模板数 = `references/<族>/` 下 .h/.cpp/.template 实测计数（冒烟静态断言防漂移）；
状态 = ✅ 实质内容 / 🟡 骨架（索引层）/ ❌ 占位。

| 族 | 典型算子 | 架构 | 模板数 | 状态 | 文档 |
|----|---------|------|-------|------|------|
| matmul | MatMul/BatchMatMul/MXFP4/A16W16 | DAV_3510 | 1 | ✅（Blaze 基础模板 ×1，回流自 blaze skill 480-case 穿刺验证） | `references/matmul/guide.md`（六策略互斥判定树）+ `references/matmul/templates/` |
| mc2 | matmul_all_reduce/allgather_matmul/alltoall_matmul | DAV_3510 | 0 | ✅ 设计参考（无货架代码） | `references/mc2/guide.md`（3 策略） |
| reduce | ReduceSum/Softmax/LayerNorm/RMSNorm | DAV_3510 | 10 | ✅（Softmax 模板 ×8 + kernel_utils + tiling） | `references/reduce/guide.md` |
| elementwise | Sin/Cos/Abs/Exp | DAV_3510 | 5 | ✅（add_custom 可裁剪工程模板：kernel .asc/.h + host .asc + torch 扩展 ×2，回流自 direct-invoke-template；scripts/ 验证脚本不计模板数） | `references/elementwise/guide.md` + `references/elementwise/templates/`（usage_guide + add_custom） |
| broadcast | 含广播轴 Add/Mul/Sub | DAV_3510 | 5 | ✅（code/ 可裁剪源码） | B 型选型（OneDim 快路径 + NDDMA/DataCopyPad/UB 三实现） |
| conversion | Transpose/Concat/Split | DAV_3510 | 11 | ✅（transpose 模板 + tiling_data） | `references/conversion/`（12 篇 transpose） |
| sort | TopK/KthValue/Sort/ArgSort | A2+A5 | 0 | 🟡 设计参考（M-way 归并三 Pattern 路由 + Pattern C 四阶段架构已蒸馏；radix 路线 radix_sort.md；无货架代码） | `references/sort/guide.md`（Pattern 决策树）+ `references/sort/radix_sort.md` |
| scalar | ScalarBound 算子 | A2+A5 | 0 | ✅（设计原则 + conv 实践，无模板） | `references/scalar/guide.md` |
| simt | 条件分支/离散索引 | DAV_3510 | 0 | 🟡 设计参考（VF 三件套范式 + 共享内存/多核同步 + verdict 映射已蒸馏；无货架代码） | `references/simt/guide.md`（编程范式）+ `references/simt/optimization-guide.md` |
| fa | FlashAttention 类 | DAV_3510 | 0 | 🟡 设计参考（方法论已蒸馏：三通道 Roofline + mix kernel 设计 + 不变量 I1-I5；无货架代码，实战案例仍走降级策略） | `references/fa/guide.md`（判定映射 + 区制路由）+ `references/fa/fa_roofline_design.md` + `references/fa/fa_mixed_kernel_design.md` |
| convolution / nn / random | — | — | 0 | ❌ 规划中（M2+） | — |

未收录族返回「该算子族优化知识暂未收录」并**计数上报**（知识缺口感知回路的输入，
落盘 `references/knowledge_gaps.json`）。融合算子拆解为基元族分别查询
（AddRelu = broadcast + elementwise）。

## 模板代码查找链路

```
本 SKILL.md 分类表
  → references/<family>/guide.md（族指南）
    → 模板表选方案（适用条件列）
      → references/<family>/templates/<模板名>.md（说明文档）
        → 同名 .h / .cpp / .template（代码骨架，以此为实施基线）

跨族公共设施：references/common/（计时 harness 货架——
  timing_harness/ 两模板 + index.md，snapshot kernel_avg_us 的第一生产者，
  见 P0-2；搬运/尾块/UB 常驻设计参考）
```

## 脚本

| 脚本 | 职责 |
|------|------|
| `scripts/family_asset_audit.py` | 族表模板数静态断言（防 SKILL.md 数据漂移，CI 门禁） |
| `scripts/tiling_candidates.py` | Tiling 候选生成器·四族（elementwise/reduction/broadcast/conversion）：shape/dtype + soc-spec 容量约束 → 候选参数表（block_dim/tile），逐候选附容量占用计算过程 + 剪枝记录——机制卡「禁止经验值交付」的执行载体（P0-5；L2_UNDERUTILIZATION 命中后调用的首选工具） |

## 模板优先纪律（T1-T5，编排层同款）

1. 查到模板（.h/.cpp/.template）→ **直接拷贝到项目 `op_kernel/`**（保留 dav310 等子目录
   结构），仅做最小适配；`.template` 先按 usage_guide 转同名 .h/.cpp。
2. 模板内 MicroAPI VF 计算（RegTensor/MaskReg/LoadAsFp32/StoreFromFp32）**原样保留**，
   禁止降级为高层 API。
3. MicroAPI 编译报错修复顺序：①命名空间别名（`namespace MicroAPI = Reg;`）→ ②API 签名
   适配（对照 CANN 实际头文件）→ ③include 路径 → ④全部无效才降级**对应分支**，
   并在结果中列出尝试过的修复与失败原因。
4. 实施方返回「模板使用情况」：哪些直接拷贝、哪些降级、降级原因与尝试记录。

## 文档结构（A/B 双型）

- **A 型 单实现调参**（如 matmul pingpong/swat）：优化目标（量化收益）→ 架构概览 →
  关键参数 → 核心循环（改造前后对照含事件同步）→ 关键修改点表。
- **B 型 多实现选型**（如 broadcast）：总览速查 + **选型决策树**（决定性能的核心）+
  每实现一份分册 + code/ 可裁剪源码。选 B 时收益 = 选对实现避免劣化，必须给选型依据。

## 扩展新族

1. 建 `references/<family>/`；2. 判 A/B 型按对应结构写；3. 更新本表；4. 族 guide 内
   模板与判定的 mechanism_refs 对齐（新增机制卡时反向引用）。
