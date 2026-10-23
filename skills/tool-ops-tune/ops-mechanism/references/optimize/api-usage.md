# 机制卡：API 使用优化（标量削减 / Cast 削减 / 融合指令）

> 对应判定规则：L1_SCALAR_BOUND、L1_VEC_BOUND（Cast 重反证）、L0_ICACHE_MISS、
> L2_HEAD_OVERHEAD。内容沉淀自标量与 API 层调优一线实践。
> **DSL 适用域（P2-5/G19）**：本卡实施细节以 **Ascend C** 为准；Triton / CATLASS / TileLang 的等价原语与实施差异见 [index.md 跨 DSL 原语对照表](index.md#跨-dsl-原语对照六原语--dsl)——跨 DSL 实施时以该表为路由、对应 DSL 专项文档为深度参考，不按本卡字面照搬。

## 标量削减（L1_SCALAR_BOUND）

| # | 方法 | 说明 |
|---|------|------|
| 1 | 缩小 TilingData | 字段越少 host→device 搬运越快 |
| 2 | 减少核数 | 数据量小时少核 = 少头开销（与 A2 满核 ~20-21us 头开销权衡） |
| 3 | TPipe 外置 | 避免 TPipe 对象内创建初始化 |
| 4 | 循环不变量外提 | 移到 Host 侧或 kernel 顶层 |
| 5 | 标量参数向量化 | 常量标量用 `Muls`/广播替代逐元素标量路径 |
| 6 | 地址计算优化 | areg 地址生成（LayerNormV3 案例：原文未附数字，机制见 cases/） |

scalar >90% 且 vec/mte 低 → 标量访问/地址计算主导：查指针运算与分支，而不是数据量。

## Cast 削减（L1_VEC_BOUND + fp32 ratio 高反证）

- 检查 `aiv_vec_fp32_ratio` vs `aiv_vec_fp16_ratio`：fp32 远高于预期 = 大量 fp16→fp32 转换；
- 精度允许 → 直接 fp16 计算（**语义变更类**：须精度代价标注 + 边界位型验证）；
- 必须转换 → 批量一次转换，禁止循环内逐元素 Cast。

## 融合指令

mul+add → Axpy 类、mul+sub 等可融合序列查表合并；`Compare+Select` 链合并。

## 头开销（L2_HEAD_OVERHEAD）

Task Duration − max(aic,aiv) 占比 >30%：数据量太小或核数太多——先减核再谈其它。

## 减少计算量（深度 VEC BOUND 核心策略，沉淀自一线调优实践）

VEC >80% 时微架构调整（DoubleBuffer/融合指令/Bank Align）收益有限——瓶颈在
计算量本身，须从**算法层减少向量指令数**：

| 手段 | 说明 | 适用条件 |
|------|------|---------|
| **LUT + 插值** | 查表+插值替代 Taylor 展开（超越函数每元素数十条 FMA 是计算量大头） | `aiv_vec_ratio` >90% 且 elementwise/activation 类 |
| 减少超越函数项数 | Taylor 12→8 项 | ❌ 有精度风险，不推荐 |

**Exp 案例（公开 API 路径）**：`AscendC::Exp<T, expandLevel, isReuseSrc>` 的
`expandLevel = 0` 即内部走查表+插值——无需手写查表逻辑。Log/Tanh 等同理，
按 asc-devkit 对应 API 的模板参数取查表模式。

**检查方法**：trace 里 `vec_exp`/`vec_log`/`vec_tanh` 等超越函数指令占比
（trace_bubble 的空泡分类 + 指令统计）；profiling 里 `aiv_vec_ratio` >90% +
算子含超越函数 → 本策略优先级最高。

## Matmul API 常量折叠（scalar bound 的 matmul 场景，蒸馏）

Kernel 使用 Matmul API 且 `aic_scalar_ratio` ≥ 0.15、shape 可固化或可取上界 →
把 `TCubeTiling` 解析迁到编译期（constexpr），降低 `aic_scalar_time`。

## 性能友好 API 选择（scalar 削减第 7 法，蒸馏）

- `SetFlag/WaitFlag` 代替 Queue、`LocalTensor` 代替 TBuffer、去除 TPipe——
  减少封装带来的 scalar 开销；
- 循环展开小循环减少分支代价；按 tiling 最简化循环轴。

## 验证与回滚

同口径 verdict 复跑（scalar_ratio / vec_fp32_ratio 下降为机制生效证据）；语义变更类
（fp16 计算）精度用例必附。噪声带内 → revert。

## 模板资产

`ops-family:references/scalar/guide.md`、`ops-family:references/elementwise/vector_efficiency_design.md`。
