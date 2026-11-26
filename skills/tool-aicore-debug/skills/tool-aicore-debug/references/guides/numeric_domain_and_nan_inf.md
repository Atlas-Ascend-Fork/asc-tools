# 除零、初等函数定义域、FP32 中间累加与 Cube 状态复位指南

## 一、触发指纹与子例对比

- **典型报错与现象**：`errcode=3`（`ccu_div0` 标量除零）、`errcode=9`（`cube_invld_input` 矩阵异常输入），或 `exit_code=0` 但输出出现 `NaN` / `Inf`。
- **过/崩子例对比**：常规小数值输入通过；遇到 `group/num == 0`、输入含 `0` 或负数的 `Ln`/`Sqrt`/`Rsqrt`、`half` 大数值累加超 `65504` 上溢变 `Inf`（再相减如 Softmax `exp(x - max)` 变 `NaN`）、或多阶段 `workspace` 残留脏 `NaN` 时出错。
- **30 秒源码搜索词**：搜 `/`、`%`、`Div`、`Reciprocal`、`Sqrt`、`Rsqrt`、`Ln`、`Exp`、`Cast`、`cmatrixInitVal`、`SetCtrl`、`GetSysWorkSpacePtr`。

## 二、必查参数与公式清单

1. **标量与向量除零及零长度空张量前置处理（防 `errcode=3` 与 `Inf/NaN`）**：
   - 主控入口检查分组数、缩放因子、池化核面积等属性：非法零或负属性（`group <= 0`、`eps < 0`）直接返回参数错误码；合法零元素空张量（`numel == 0`）在主控直接提前返回成功，不启动核函数（防止核内分核除数为 0）；
   - 核内归一化或倒数计算按算子数学定义在分母加 `eps`（如 `Adds(varBuf, varBuf, eps, len)` 后再 `Rsqrt`）。
2. **初等函数定义域与半精度升精度累加**：
   - `Sqrt` / `Rsqrt` 输入必须非负（`Rsqrt` 须 `> 0`），`Ln` 输入必须 `> 0`；尾块未使用的对齐槽位在执行 `Div`/`Ln`/`Sqrt` 前必须填入合法正数（如 `1.0f`），绝不能让尾块 `0` 或垃圾负数进入无掩码初等函数；
   - `half` 最大有限值仅 `65504`，凡涉及求和归约、平方和、`Exp` / `Softmax` 的中间计算一律先 `Cast` 到 `float`（FP32）完成计算，写回前再 `Cast` 回目标类型。
3. **矩阵乘（Cube）首拍清零、累加 `workspace` 清零与状态寄存器复位（防 `errcode=9` 与脏值扩散）**：
   - K 维分块循环首次调用 `Mmad` 必须确保 `cmatrixInitVal = true`（清零 L0C 旧值），后续分块设为 `false`；
   - 使用全局 `workspace` 做多核原子累加时，启动累加核前必须先把 `workspace` 清零（如主控 `aclrtMemsetAsync` 或前置清零核）；
   - 修改过 `SetAtomicAdd` 或饱和控制位 `SetCtrl` 的核函数，退出前必须恢复 `SetAtomicNone()` 与初始控制位；若算子本身要求透传输入中的 `NaN/Inf`，不得擅自把合法输入清洗为 `0`。

## 三、常见误区（避坑）

- **误区 1**：尾块求均值时只屏蔽了 `ReduceSum`，随后在整块上调 `Ln(buf)` 或 `Div(a, buf)`，结果尾块补 `0` 槽位产生 `-Inf` / `NaN` 并在后续搬运中污染输出。
- **误区 2**：为了消掉 `NaN` 把输出张量强行钳制为常数，破坏正常样本的数学精度。

## 四、标准修复代码模板

```cpp
// 1. 合法空张量主控直接早退成功，非法零属性直接报错拦截（op_api / op_host）
if (eps < 0.0f || group <= 0) return ACLNN_ERR_PARAM_INVALID;
if (totalElements == 0) return ACLNN_SUCCESS; // 空张量不启动核函数，避免核内分片除零

// 2. 核内 half 升 FP32 归约 + 分母加 eps 防除零（op_kernel）
AscendC::Cast(xFp32, xHalf, AscendC::RoundMode::CAST_NONE, validLen);
AscendC::PipeBarrier<PIPE_V>();
// ... 执行 FP32 求均值与方差 varFp32 ...
AscendC::Adds(varFp32, varFp32, eps, validLen);
AscendC::PipeBarrier<PIPE_V>();
AscendC::Rsqrt(rstdFp32, varFp32, validLen);
```
