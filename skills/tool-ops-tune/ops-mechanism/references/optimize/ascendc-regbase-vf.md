# 机制卡：RegBase / SIMD VF（A5 向量范式）

> 对应判定规则：L1_VEC_BOUND（深度档：>80% 接近极限后的存量空间）、多步融合 Vector、
> MemBase 已尽场景。仅 A5（dav-3510）；A2/A3 保留兼容分支，禁止下沉此路径。
> 内容沉淀自 A5 RegBase/VF 与 SIMT 范式调优一线实践。
> **DSL 适用域（P2-5/G19）**：本卡实施细节以 **Ascend C** 为准；Triton / CATLASS / TileLang 的等价原语与实施差异见 [index.md 跨 DSL 原语对照表](index.md#跨-dsl-原语对照六原语--dsl)——跨 DSL 实施时以该表为路由、对应 DSL 专项文档为深度参考，不按本卡字面照搬。

## 原理

A5 的 RegBase 编程范式把数据驻留寄存器（RegTensor），以 SIMD VF（Vector Function）指令
直接计算，绕开 UB 往返与队列同步。适用：多步短生命周期计算（量化/激活/归一化组合）、
UB 带宽已成约束的 Vector 算子。

## 适用前提

- 目标 SoC = A5（编译目标 ascend950 / dav-3510）；A2/A3 必须保留原实现分支；
- 多步计算间数据生命周期短（适合寄存器驻留）；
- UB 预算紧张（寄存器驻留缓解 UB 压力）或 Vector 指令链长。

## 关键结构（模板必须原样保留的部分）

RegTensor / MaskReg / LoadAsFp32 / StoreFromFp32 等 MicroAPI VF 计算结构——
**模板内这些结构禁止降级为高层 API**（模板拷贝纪律，见 ops-family）。

## 编译修复顺序（MicroAPI 报错时，禁止直接降级）

1. 命名空间别名（`namespace MicroAPI = Reg;`）
2. API 签名适配（对照 `$ASCEND_HOME_PATH/.../reg_compute/` 实际头文件）
3. include 路径修正
4. 全部无效才降级**对应分支**，并在结果中列出尝试过的修复与失败原因

## 验证与精度风险

- 数值稳定性：VF 计算的舍入行为与高层 API 可能不同（如求逆 VF 化案例：须覆盖数值
  稳定性验证）→ 精度用例含边界值/异常位型；
- 判定证据复采：vec_ratio / vec_fp32_ratio 变化 + wall 收益（event 口径）。

## 回滚

编译修复三连无效且降级后无收益 / 精度位型失败 → revert 整个 VF 分支（保留兼容分支）。

## 模板资产

`ops-family:references/simt/optimization-guide.md`（SIMT 不规则读写）、
`ops-family:references/elementwise/vector_efficiency_design.md`。
