# 跨 detector 的证据、容量与单位约定

本文件约束 diagnose 的规则解释与修复，优先于历史种子摘要中的简化表达；lint 的结构检查定义仍见 `mechanical_checks.md`。

## 容量与单位

- 区分芯片物理容量、编译器可分配容量、某个 tensor 实际分配大小和本次操作可访问的剩余范围。不能用物理容量替代 tensor 的分配边界。
- 先确认目标架构、CANN/编译器版本、API 重载、数据类型、布局和实际分配范围，再把偏移、长度、stride 换算成字节比较。
- 历史 detector 中的 UB 192KiB、L0A/L0B 64KiB、L0C 128KiB 只记录对应种子的阈值，不是跨型号常量；是否适用于当前输入须由环境资料确认。未知时记 `runtime_required=true`，不得根据历史阈值直接定罪或自动改小参数。
- `KiB = 1024 bytes`；float32 元素占 4 bytes。例如 40960 个 float32 为 160KiB，32768 个为 128KiB。

## 普通 DataCopy 的分块跨度

本节仅适用 GM↔UB 普通 `DataCopy(..., DataCopyParams)` 分块搬运，字段单位均为 32-byte dataBlock；不适用于 DataCopyPad、切片、ND2NZ、NZ2ND 等重载。后者必须按各自文档确认字段单位及 padding/alignment。

设 `count = blockCount`、`len = blockLen`、`gap = srcStride` 或 `dstStride`，且 count、len 为正：

```text
span_bytes = (count * len + (count - 1) * gap) * 32
offset_bytes + span_bytes <= allocation_bytes
```

gap 是前一块尾到后一块头的间隔，不是两块起点间距。gap=0 表示紧密排列；count=1 时 gap 不影响跨度。对源、目标分别计算，不得只检查其中一侧。运算使用足够宽的整数并检查溢出。

依据：[CANN DataCopy 通用搬运参数定义](https://www.hiascend.com/doc_center/source/zh/CANNCommunityEdition/82RC1alpha003/API/ascendcopapi/atlasascendc_api_07_0102.html)。[DataCopyPad 参数定义](https://www.hiascend.com/doc_center/source/zh/canncommercial/850/API/ascendcopapi/atlasascendc_api_07_0265.html)展示了不同重载的单位差异，不能直接套用本公式。

## padding 与 filter

- 对普通对称 padding，几何条件为 `H + 2*pad >= filterH` 且 `W + 2*pad >= filterW`。增加非负 padding 不会让这个有效范围变小。
- 所需下限取 H/W 两方向的 `ceil(max(filter - fmap, 0) / 2)` 的最大值。上限来自目标 API、字段范围和算子定义，不能自行假设为 H/2 或 W/2。
- 若下限大于上限，配置不可行；返回失败并调整 tiling/filter/shape，不能调用下限大于上限的 clamp。
- 参数调整可能改变算子含义。优先校验和报告；只有目标语义允许时才采用候选 padding，禁止为消除报错静默改写 padding。

## 实验目标与实际覆盖

- 正式 ID 标识 detector；原始 L 编号等记录在 `硬件事件编号`，不是路由主键。`kernel_diff_type` 只取映射表中的枚举，具体差异记录在 `kernel_diff_note`。
- 目标异常、源码风险和实际硬件上报是三种证据。OOB 兜底报错只能证明兜底路径，不能确认非法指令、负数开方、Dcache response 等原始目标。
- Step 2 的异常命中率只计入能够区分该异常的规则；通用 OOB 兜底、候选区分说明、纯监控配置及仅缺断言的防御建议作为辅助信息，不单独支撑根因或进入异常命中率。若具体越界值已确认，则按相应根因规则计分。
- 编译器隐式同步必须有适用于当前 API 层级、编译器版本及生成指令的证据。不能仅因出现 LocalTensor 或 SetFlag 就宣称所有冲突不可达；也不能仅因缺显式屏障就断言硬件一定冲突。
- 当种子源码、日志或 ISA 依据缺失/冲突时，保留候选并标注证据不足，不臆造访问通道、容量、错误码或验证结果；不得据此执行不确定的源码替换。
