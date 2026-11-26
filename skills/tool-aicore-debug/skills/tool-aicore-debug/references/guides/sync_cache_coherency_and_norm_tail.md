# 标量写 UB/GM 缓存刷写、Vector 流水栅栏与尾块归约精度指南

## 一、触发指纹与子例对比

- **典型表现**：`exit_code=0` 无硬件报错，但输出张量或副输出（如 `mean` / `rstd`）出现较大相对误差、`NaN` 或随机旧值残留。
- **过/崩子例对比**：
  1. 64 元素整倍数的维度通过，非 64 对齐维度（如 `D = 96`、`100`）归约均值/方差严重偏大或变 `NaN`；
  2. 纯 Vector 计算分支正常，一走到含标量 `SetValue` / 下标改写 UB 槽位再转 Vector/MTE 搬运的分支就丢失改写值；
  3. 普通 `LayerNorm` 模式通过，切到 `RMS_NORM` 模式时 `mean` 副输出含未初始化脏数据。
- **30 秒源码搜索词**：在 `op_kernel/` 中搜 `SetValue`、`__ubuf__`、`Dcci`、`DataCacheCleanAndInvalid`、`Duplicate`、`ReduceSum`、`DoSum`、`PipeBarrier`、`mean`。

## 二、必查参数与公式清单

1. **标量改写 UB 后必须刷缓存再交由 Vector / MTE 读取**：
   - AI Core 的标量单元（Scalar）读写走独立数据缓存（D-Cache），而 Vector 与 MTE 直接访问物理 UB；
   - 凡用标量语句（如 `ubTensor.SetValue(idx, val)` 或指针赋值 `ubPtr[idx] = val`）修改 UB 内容后，在后续 Vector 计算或 MTE `DataCopy` 读取该 UB 前，必须调用 UB 数据缓存刷写（如 `DcciUBImpl` 或 `DataCacheCleanAndInvalid`）并插入 `PipeBarrier<PIPE_V>()` / `PipeBarrier<PIPE_ALL>()`。
2. **标量写 GM 与 MTE / 原子累加混用的缓存一致性**：核函数用标量指令直接写 `__gm__` 后，若紧接着对同一缓存行做 MTE 搬运或 `SetAtomicAdd` 写回，必须先对目标 GM 地址执行 `DataCacheCleanAndInvalid<T, CacheLine::SINGLE_CACHE_LINE, DcciDst::CACHELINE_OUT>` + `PipeBarrier<PIPE_ALL>()`，防止脏缓存行覆盖原子结果。
3. **Vector 清零初始化与循环累加之间的 `PipeBarrier<PIPE_V>()`**：调用 `Duplicate(sumBuf, 0.0f, len)` 初始化累加缓冲后、进入循环 `Add(sumBuf, sumBuf, tileBuf, ...)` 前，必须显式补齐 `PipeBarrier<PIPE_V>()`。
4. **非对齐尾块归约掩码与模式副输出完整性**：
   - `DoSum` / `ReduceSum` 处理不足一个 Vector repeat（`float` 为 64 元素，`half` 为 128 元素）的尾块时，必须先把对齐补齐槽位清 `0`（或设精确 `SetVectorMask`），严禁把尾块后面的未初始化 UB 脏值一并求和；
   - 融合算子多模式分支（如 `RMS_NORM` 不需要计算均值，但接口输出仍带非空 `meanOut` 张量）须确认接口约定：若测试检查 `meanOut`，须在非空时零初始化写回 GM。

## 三、常见误区（避坑）

- **误区 1**：只修了 `Duplicate` 后的 `PipeBarrier<PIPE_V>()` 和 `meanOut` 写回，漏掉了标量 `SetValue` 写 UB 后的 `DcciUBImpl` 刷缓存，导致特定形状下标量修正值在硬件上偶发或必现不生效。

## 四、标准修复代码模板

```cpp
// 1. Duplicate 初始化累加器 + PipeBarrier<PIPE_V>() + 非对齐尾块掩码求和
AscendC::Duplicate<float>(sumLocal, 0.0f, alignElems);
AscendC::PipeBarrier<PIPE_V>();
// 若 validElems < alignElems，先把 [validElems, alignElems) 槽位清零再归约
for (uint32_t i = validElems; i < alignElems; ++i) {
    tailBuf.SetValue(i, 0.0f);
}
// 2. 标量改写 UB 后刷出 D-Cache，再由 Vector / MTE 消费
AscendC::DcciUBImpl<uint32_t, AscendC::CacheLine::ENTIRE_DATA_CACHE,
                    AscendC::DcciDst::CACHELINE_OUT>();
AscendC::PipeBarrier<PIPE_V>();
AscendC::Add(sumLocal, sumLocal, tailBuf, alignElems);

// 3. RMS_NORM 等分支对非空可选副输出做零填充写回
if (isRmsNorm && hasMeanOut) {
    AscendC::Duplicate<float>(zeroBuf, 0.0f, rowCountAligned);
    AscendC::PipeBarrier<PIPE_V>();
    CopyOutMean(zeroBuf, rowCount);
}
```
