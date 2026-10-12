# 算子与实际计算链

算子说明先固定数学 reference、I/O、dtype、layout、有效区和实际计算链，再路由版本 API。当前覆盖 Reduce、Broadcast/Transpose、Sort/Merge、Matmul/MMAD、Normalization、Convolution 与 SoftplusV2Grad/SoftplusBackward 算法合同。文中的 checkpoint 只是候选观察点；没有独立 oracle 时不能据此声称定位。

- [SoftplusV2Grad / SoftplusBackward 算法合同](softplus-v2-grad.md)：以原始 `beta*self` 的分段谓词、输入角色和 CopyOut 最后 writer 定位梯度分支失配；不代表产品 API 支持。

## 资料导航

按当前现象选择排查方法。具体 API 的产品、类型和版本限制调用 CANNBOT `ascendc-docs-search` 技能查询，也可搜索示例代码和类似算子；表中的提示帮助选择需要先核对的条件。

| 资料位置 | 何时读 / 解决什么 | 使用提示 |
| --- | --- | --- |
| [归约算子族](reduction.md) | 核对参与元素、带索引布局、pattern/临时区、Atomic 状态及跨核跨卡合并顺序。 | 先核对参与元素，再检查累加与合并顺序 |
| [广播与转置算子族](broadcast-transpose.md) | 核对逻辑广播与物理重排，并检查小通道补齐、有效输出和量化转换链。 | 区分逻辑广播与物理重排 |
| [排序与归并算子族](sort-merge.md) | 核对有序范围、value/index、有效路与消耗计数、偏移单位、并列规则和 TopK 边界。 | 保留有效长度、并列规则和输出数量 |
| [Matmul/MMAD 算子族](matmul-mmad.md) | 检查分组与分块、数据/scale 装载、转置、异步写回、HF32 及量化后处理。 | 区分基础指令、高阶接口与实际分块路径 |
| [SoftplusV2Grad / SoftplusBackward 算法合同](softplus-v2-grad.md) | 以原始 beta*self 分段谓词、gradOutput/self/beta/threshold/gradInput 角色和 CopyOut 最后 writer 定位 Softplus 梯度分支失配；不代表产品 API 支持。 | 按当前 beta、threshold 和输入范围展开分支 |
| [Norm 算子族](normalization.md) | RMSNorm/LayerNorm 逐段检查统计量归约、方差稳定性、epsilon 位置、开方/倒数、缩放与偏置，避免用数学定义变化掩盖异常。 分块路径另查 Welford 统计状态及在线 Softmax 的共同基准与重缩放。 | 统计量、epsilon 和合并公式按算子定义核对 |
| [Convolution 算子族](convolution.md) | 把卷积拆为 padding、im2col/LoadData、group/dilation 通道归属、K 参与集合、Cube/MMAD 累加和后处理；从输出坐标反解逻辑窗口并用单点激活证伪跨组或膨胀映射错误。 | 结合窗口、分组、padding 和搬出路径检查 |
| [动态量化：统计范围、scale 角色与舍入边界](dynamic-quant.md) | DynamicQuant/V2 沿分组统计、scale 与倒数、offset、浮点映射、Cast/饱和和辅助参数写回定位；用半整数邻域区分小浮点误差如何改变整数输出。 | 分别核对统计范围、scale 角色与量化边界 |
