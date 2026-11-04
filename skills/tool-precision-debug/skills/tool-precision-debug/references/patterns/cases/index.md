# 案例与历史调查线索

按当前异常选择一条排查路线。案例中的输入、版本和执行方式各有范围；使用时先匹配当前条件，再用自己的失败输入验证。

| 当前问题 | 参考案例 | 使用提示 |
| --- | --- | --- |
| 最终已是 FP32，小增量仍然消失 | [FP16 升精度位置](fp16-promotion-boundary.md) | 固定输入的加法与 Cast 示例；运行环境见正文 |
| 整数转浮点后发生判重或寻址异常 | [相邻整数碰撞](integer-float-collision.md) | 检查首次丢失整数身份的位置；运行环境见正文 |
| 尾块、零工作量或跳过分支后读到旧值 | [尾块有效区与旧状态](tail-valid-region-state.md) | 用状态模型区分本轮未写、有效长度和复用问题 |
| 单块统计正确，合并后失配 | [Partial 统计合并](partial-statistics-merge.md) | 用可手算的分块模型区分漏项、权重和合并公式 |
| 逐元素通过但 bias 失败，主列与尾列或 NaN/Inf 分叉 | [GRU p2/p3 的 bias 归约](thnn-fused-gru-bias-reduction.md) | 区分参考结合树、尾列分派与修复中新引入的计数漏写；保留历史验证范围 |
| 事件看似配对，多轮或多输出仍异常 | [同步诊断](sync-dataflow-handshake.md) | 代码分析示例；区分区域绑定、条件通知和缓冲回收 |
| 融合参数失效或输出像另一个中间量 | [FusedRmsNormGeluResidual](fused-rmsnorm-gelu-residual.md) | 历史排查示例；先查角色绑定，再查临时区和跨 tile 状态 |
| 梯度输出、极值或 Host 属性出现异常 | [SoftplusV2Grad](softplus-v2-grad-family.md) | 历史排查示例；根据错值选择分支、精度或属性复用路径 |

具体实验方法见[诊断方法](../../diagnosis/index.md)。修复后回放原失败输入，再按改动范围选择回归；症状相似不代表可以直接复制常量或补丁。
