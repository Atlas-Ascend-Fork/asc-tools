# Elementwise 族

| 文档 | 内容 |
|------|------|
| [guide.md](guide.md) | **族指南（入口）**：verdict 命中 → 机制路由表 + tiling 候选生成 + 模板货架入口 |
| [templates/add_custom/](templates/) | **可裁剪工程模板**：双缓冲 kernel + host + torch 扩展（usage_guide 转换纪律） |
| double_buffer_design.md | 双缓冲设计（对应 L1_SERIAL_NO_OVERLAP） |
| vector_efficiency_design.md | 向量化效率（对应 L1_VEC_BOUND / 减 Cast） |

内容沉淀自 Elementwise 类算子调优实践。
