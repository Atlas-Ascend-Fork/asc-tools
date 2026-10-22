# Elementwise 模板使用指南（usage_guide）——add_custom 可裁剪工程模板

> `templates/add_custom/` 的转换与适配说明。模板拷贝纪律（SKILL.md T1-T5）
> 同样适用：**直接拷贝 + 只改 `[MODIFY]` 点**。

## 模板来源与验证状态

回流自 Ascend C Kernel 直调工程模板（Vector 样例工程）
（经真机验证的 Vector 样例工程：双缓冲 TQue + DataCopyPad 尾块处理 +
多核均分 + 尾核特化 + torch 扩展双通路）。与本仓真机 E2E 走查同款工程形态
（square_add@910B3）。

## 目录结构

```
add_custom/
├── op_kernel/
│   ├── add_custom_tiling.h    Tiling 常量 + 结构体（kernel 和 host 共用）
│   └── add_custom_kernel.asc  纯 kernel（KernelAdd 类 + 核函数入口，[MODIFY] 标记）
├── op_host/
│   ├── add_custom.asc         Host + main 入口（直调通路）
│   └── data_utils.h           数据读写工具
├── op_extension/
│   ├── add_custom_torch.cpp   PyTorch host 实现（Tiling 计算 + launch）
│   ├── register.cpp           TORCH_LIBRARY 注册
│   └── ops.h                  函数声明
├── CMakeLists.txt             双 target：可执行文件 + libadd_custom_ops.so
└── scripts/                   gen_data / golden / verify / test_torch
```

## 转换步骤

1. 整目录拷贝入工程，改名为 `<your_op>`；
2. 只改 `[MODIFY]` 标记点（kernel 头注释列了 5 处：include 名 / 类名 / 核函数名 /
   计算逻辑（`AscendC::Add` → 你的算子）/ 输入输出数量）；
3. **计时结构不许改**：`InitBuffer(que, DOUBLE_BUFFER, TILE_LENGTH * sizeof(T))`
   的双缓冲、`CopyIn/Compute/CopyOut` 三段循环、尾块 `DataCopyPad` 处理——
   改了口径，前后对比失效（timing_harness 同款纪律）；
4. TILE_LENGTH 按 tiling 候选调整：
   `python3 ops-family/scripts/tiling_candidates.py --family elementwise
   --shape SHAPE --dtype DTYPE --soc SOC`（容量预算校验内置，SHAPE/DTYPE/SOC
   为实际值占位）。

## 已内置的正确结构（不要自己重造）

| 结构 | 位置 | 说明 |
|------|------|------|
| 多核均分 + 尾核特化 | Init 的 blockIdx 分支 | 非尾核 numPerCore / 尾核 tailNumLastCore |
| 双缓冲流水 | InitBuffer DOUBLE_BUFFER | L1_SERIAL_NO_OVERLAP 的机制基线 |
| 尾块对齐 | DataCopyPad + tailTileElementNum | 非 32 对齐 shape 不崩 |
| 精度验证链 | scripts/{gen_data,golden,verify_result} | 工程现有容差（禁止自创） |
| torch 双通路 | op_extension/ | torch.ops.npu 调用 + 与直调结果对拍 |

## 性能调优挂钩（本仓链路）

- 计时：无自带 benchmark 时拷
  `ops-family/references/common/timing_harness/`（测量设施非优化变量）；
- 判定：采集后 facts_bridge → verdict（MTE2/VEC/SCALAR 命中 → 见族 guide
  [guide.md](../guide.md) 路由表）；
- tiling 候选：tiling_candidates.py（elementwise 族四约束内置：
  多核 512 元素对齐 / UB 256B 对齐 / 每核 ≥4KB / fp32 中间精度分支）。
