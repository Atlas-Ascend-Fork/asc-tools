---
name: ops-family
description: 算子族实践库与模板代码货架。按算子族（matmul/mc2/reduce/elementwise/broadcast/conversion/sort/scalar/simt…）组织的实现范式、A/B 双型文档与可拷贝代码模板（.h/.cpp/.template）。实施环节的模板优先纪律（直接拷贝 + 最小适配 + 编译修复顺序）在本子域落地。触发：查算子族优化参考实现、查模板代码、实施方案落地时使用。
---

# 算子族实践库（family）

> **本子域知识资产（族 guide / 模板 / tiling 候选生成器 / 资产断言）随「算子族
> 模板知识」PR 交付**，当前为骨架占位——SKILL.md 先行声明模板优先纪律与查找
> 链路，供方案卡 `template` 字段的前向引用对齐。

## 模板优先纪律（T1-T5，编排层同款）

1. 查到模板（.h/.cpp/.template）→ **直接拷贝到项目 `op_kernel/`**，仅做最小适配；
2. 模板内 MicroAPI VF 计算（RegTensor/MaskReg/LoadAsFp32/StoreFromFp32）原样保留，
   禁止降级为高层 API；
3. 编译报错修复顺序：命名空间别名 → API 签名适配 → include 路径 → 全部无效才
   降级对应分支并留痕；
4. 实施方返回「模板使用情况」：哪些直接拷贝、哪些降级、降级原因与尝试记录。

## 查找链路（资产合入后生效）

套件根 SKILL.md 子域路由表 → `references/<family>/guide.md`（族指南）→ 模板表
选方案 → 模板代码。未收录族返回「该算子族优化知识暂未收录」并计数上报
（knowledge_gaps.json，缺口感知回路输入）。
