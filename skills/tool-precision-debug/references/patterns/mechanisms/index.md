# 精度异常机制

本区域沉淀可跨算子复用的精度异常机制，包括触发现象、成立条件和区分实验。初筛时，根据计算链上的 Cast、归约、索引、搬运、同步、缓冲复用或尾块等敏感点查阅；取得中间观察后，再聚焦首次分叉及其上游依赖，不要求先找到异常边界才能使用这些经验。

机制页说明常见现象、成立条件和检查方法。API 签名、支持类型、硬件容量与同步范围查询当前 CANN 的官方文档和平台说明。

症状相似只能提供线索；用对照实验确认原因，修复后回放失败输入并做必要回归。

## 按症状打开案例

尾块或条件分支后读到旧值时，先查 [尾块有效区与旧状态](../cases/tail-valid-region-state.md)；单块正确、合并 partial 后失配时，查 [Partial 统计合并](../cases/partial-statistics-merge.md)。融合参数失敏或临时区疑似覆盖时，查 [FusedRmsNormGeluResidual](../cases/fused-rmsnorm-gelu-residual.md)；错值像中间量、极值或属性沿用时，查 [SoftplusV2Grad](../cases/softplus-v2-grad-family.md)。整数索引/计数进入浮点后才相等时查 [相邻整数碰撞](../cases/integer-float-collision.md)；小增量在最终升到 FP32 前已消失时查 [FP16 升精度位置](../cases/fp16-promotion-boundary.md)。事件配对但多轮、条件或多输出异常时，查 [同步诊断](../cases/sync-dataflow-handshake.md)。案例只提供调查分叉，不能替代机制页或当前 API 证据。

## 资料导航

按当前现象选择排查方法。具体 API 的产品、类型和版本限制调用 CANNBOT `ascendc-docs-search` 技能查询；表中的提示帮助选择需要先核对的条件。

| 资料位置 | 何时读 / 解决什么 | 使用提示 |
| --- | --- | --- |
| [搬运、对齐与有效区](data-movement-alignment.md) | 用物理行距、有效范围和地址对照检查搬运；筛选后继续核对有效数、mask 状态与输入复用。 | 区分有效长度、物理容量与搬运范围 |
| [Buffer 生命周期、绑定与别名](buffer-lifetime-alias.md) | 检查绑定、跨 tile 生命周期、队列与事件资源、共享容量和系统/用户 workspace 分区。 | 核对每轮读写区域、绑定与生命周期 |
| [流水同步与数据可见性](pipeline-sync-visibility.md) | 检查实际读写流水、数据就绪与缓冲回收，并区分缓存可见性与观察扰动。 | 先区分核内流水依赖与核间通知 |
| [Cast、舍入与中间精度](cast-rounding.md) | 定位转换前后首次信息损失、整数碰撞、覆盖与比例偏差，区分量化中间模型和原验收基准。 | 明确每一步的 dtype、舍入和转换位置 |
| [累加、归约顺序与消减](accumulation-order.md) | 区分累加 dtype、跨块合并及框架参考的分层、主尾列分派，用数值与结构边界设计对照。 | 不把简单顺序累加、高精度或局部 chunk 规律当成完整 reference |
| [NaN、Inf、溢出与次正规数](special-values.md) | 按有限数、正负零、正负 Inf、NaN 的首次类别分叉定位 producer，并沿产生、搬运、消费三段区分非法域、传播与 FTZ 候选。 | 特殊值处理按任务语义与当前接口核对 |
| [Tiling、尾块与跨 tile 状态](tiling-tail-state.md) | 核对 Host/Kernel 分派、调用计数、可变参数消耗、分批覆盖、零工作量、跨 tile 状态及平台容量预留。 | 后续输出段漏写时，区分有效长度、剩余计数与地址推进 |
| [Golden、Host 属性与构建采用](golden-host-contract.md) | 按原输出要求检查参考实现、Host 属性、缓存、构建采用和通信上下文生命周期。 | 参考实现、Host 配置和实际构建分别核对 |
| [Matmul、卷积、Norm 与复合算子边界](complex-operator-boundary.md) | 把复合算子拆成布局搬入、主计算、归约、后处理和搬出边界；用反向依赖切片与 fan-in 完整贡献表锁定最早分叉，并判断绕过后残差应留在线内还是分线。 | 分阶段建立可解释的中间结果对照 |
| [Shape、Rank 与边界维度合同](shape-boundary-contract.md) | 区分 rank-0、空维、维度 1、shape、tiling、broadcast 与动态分派，用 B-1/B/B+1、等价 reshape 和不等号审计核对 Host 与 Kernel 的边界语义。 | 区分空维、维度 1 与 rank-0 |
| [索引、Offset 与地址归属](index-address-ownership.md) | 把逻辑坐标、元素 offset、字节地址和半开所有权分层，用错值反解 delta、相邻合法位置与跨边界样本区分 ND/blocked 映射错误。 | 按实际布局、打包方式和地址单位推导 |
| [精度模式、近似路径与 Fallback](precision-mode-approximation.md) | 分离 API 近似、过早降精度、Cast 时机和下游放大，并用 dtype×tiling 二维对照识别升精度实验的路径混杂。 | 升精度时同时检查是否改变了执行路径 |
| [算法公式、常量与参数槽位](algorithm-parameter-contract.md) | 用逐操作公式、有限精度重结合、形参与实参、原始语义判据、分组 prefix 与错值反解定位公式和参数错误。 | 先明确算子定义、参数角色和分组关系 |
| [多核分区、所有权与核间协同](multicore-ownership.md) | 核对核与 rank 的任务归属、等待覆盖、通知轮次、通信缓冲和远端完成。 | 核对参与核、读写区间和每轮 workspace 状态 |
