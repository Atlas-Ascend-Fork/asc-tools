# matmul 模板使用指南（usage_guide）

> `matmul_basic_blaze.template` 的转换与适配说明。模板拷贝纪律（SKILL.md T1-T5）
> 同样适用：直接拷贝 + 只改 `[MODIFY]` 点。

## 转换步骤

1. 拷贝 `matmul_basic_blaze.template` → 工程 `op_kernel/matmul_kernel.h`
   （`.template` 后缀去掉；本模板是 markdown 载体，代码段整体拷出）；
2. 按 `[MODIFY]` 标注改：dtype 类型别名（fp16=half / bf16=bfloat16_t /
   fp32=float 且 IS_FP32=true）、ProblemShape、tiling 字段映射；
3. tiling 候选用 `ops-family/scripts/tiling_candidates.py --family matmul`
   生成（容量预算已校验），不人工拍；
4. 依赖：工程需含 ops-tensor blaze 头文件（`op_kernel/include/blaze/`，
   Blade library 路径——组件链 Kernel/BlockMmad/Scheduler/Epilogue 全部来自
   `Blaze::Gemm` 命名空间，本模板不引入 blaze_custom）。

## 验证锚点

- 来源模板已通过 480 case 穿刺（fp16/bf16/fp32 × ND/NZ × transpose × bias）；
- 拷贝后先跑最小 case（如 128×128×128）验证正确性，再上批量；
- 编译报错按 MicroAPI 修复顺序（SKILL.md T3）：①命名空间 ②API 签名
  ③include ④全部无效才降级对应分支并留痕。

## 边界

- 融合 epilogue（vector 后处理）不在此模板（BlockEpilogueEmpty）——
  融合 matmul 见 Blaze/tensor_api 融合开发场景；
- Grouped MatMul / MX 量化变体为后续规划项。
