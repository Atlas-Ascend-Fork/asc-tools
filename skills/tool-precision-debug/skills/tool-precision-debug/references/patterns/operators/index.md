# 算子与实际计算链

算子说明先固定数学 reference、I/O、dtype、layout、有效区和实际计算链，再路由版本 API。当前覆盖归约与自适应平均池化、广播/转置、排序/归并、Matmul/MMAD、Normalization、Convolution、动态量化与 Softplus 梯度。文中的 checkpoint 只是候选观察点；没有独立 oracle 时不能据此声称定位。

## 资料导航

按当前现象选择排查方法。具体 API 的产品、类型和版本限制使用 `ascendc-api-knowledge-query` 查询，示例代码和类似算子使用 `knowledge-query`；表中的提示帮助选择需要先核对的条件。

| 资料位置 | 何时读 / 解决什么 | 使用提示 |
| --- | --- | --- |
| [归约算子族](reduction.md) | 核对参与元素、带索引布局、Atomic 状态及跨核合并；自适应平均池化直接查[小窗口对照](reduction.md#自适应平均池化的小窗口对照)。 | 先列参与元素与 count，再检查累加顺序 |
| [广播与转置算子族](broadcast-transpose.md) | 核对逻辑广播与物理重排，并检查小通道补齐、有效输出和量化转换链。 | 区分逻辑广播与物理重排 |
| [排序与归并算子族](sort-merge.md) | 核对有序范围、value/index、有效路与消耗计数、偏移单位、并列规则和 TopK 边界。 | 保留有效长度、并列规则和输出数量 |
| [Matmul/MMAD 算子族](matmul-mmad.md) | 检查分组与分块、数据/scale 装载、转置、异步写回、HF32 及量化后处理。 | 区分基础指令、高阶接口与实际分块路径 |
| [Softplus 梯度](softplus-v2-grad.md#按残余症状进入案例) | 按错值、repeat/tail、求值顺序、小量归零或属性复用直达已有案例支路；不代表产品 API 支持。 | 部分修好后按残差继续，不重跑全部候选 |
| [Norm 算子族](normalization.md) | 核对方差、epsilon 与合并；统计 merge 错查[旧/新状态](normalization.md#统计状态的原地更新时序)，分母近似错查[迭代预算](normalization.md#倒数与开方的迭代预算)。 | 先确认统计输入，再检查分母与仿射输出 |
| [Convolution 算子族](convolution.md) | 把卷积拆为 padding、im2col/LoadData、group/dilation 通道归属、K 参与集合、Cube/MMAD 累加和后处理；从输出坐标反解逻辑窗口并用单点激活证伪跨组或膨胀映射错误。 | 结合窗口、分组、padding 和搬出路径检查 |
| [动态量化：统计范围、scale 角色与舍入边界](dynamic-quant.md) | DynamicQuant/V2 沿分组统计、scale 与倒数、offset、浮点映射、Cast/饱和和辅助参数写回定位；用半整数邻域区分小浮点误差如何改变整数输出。 | 分别核对统计范围、scale 角色与量化边界 |
