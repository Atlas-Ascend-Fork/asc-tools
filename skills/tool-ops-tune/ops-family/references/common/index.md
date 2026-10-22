# 公共优化能力（跨算子族）

| 优化类型 | 适用场景 | 文档 |
|---------|---------|------|
| 尾块处理（Tail Block） | 数据量不能被 tile 整除 | `common/tail_block_design.md` |
| 数据搬运（DataCopy） | 非对齐、小批量多次搬运 | `common/datacopy_optimization_design.md` |
| UB/TBuf 常驻复用与 Bank 冲突规避 | 多 tile 重复从 GM 搬运、冗余 MTE2 | `common/ub_resident_design.md` |

> 三篇为跨族公共优化能力的入口索引；正文按各篇结构建设（M1.5 补全）。
