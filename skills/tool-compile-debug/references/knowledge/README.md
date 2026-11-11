# Ascend C 知识索引

这里提供声明、历史映射和排查线索。声明存在、当前调用可编译、任务数值正确是三种不同证据；来源未记录准确版本/build或调用条件时，保持未知。源码和实际工具链证据可推翻索引建议。

| 当前问题 | 按需阅读 |
|---|---|
| 未声明符号、疑似拼写或调用层错误 | [API 存在性排查](hallucinated-api.md) |
| deprecated、迁移或签名变化 | [版本映射候选](api-version-drift.md) |
| 聚合头、逐头入口与目标条件 | [头入口与目标](soc-arch-gating.md) |
| 任务确定要迁移目标 | [2201/3510 迁移核验](arch-migration-2201-3510.md) |
| 需要改变实现且已有独立数学契约 | [算子实现参考](operator-idiom.md) |
| FP8/MX matmul 的目标能力裁决 | [FP8/MX 架构门控](fp8-mx-arch-gating.md) |
| API 分层模型（Tensor/vec/Reg/SIMT/c_api） | [API 分层](api-layering.md) |

检索时使用完整 namespace、API 层、实参/模板类型、入口、准确 CANN build 与实际目标。 `applicability=unknown`、未收录和多个候选都不是“API 不存在”的证据。 `header_visibility`只提出入口核验，不要求换 SoC 或重写算法。完整声明与来源调用 CANNBOT `ascendc-docs-search` 技能来检索；引擎结构化记录（含无效提取审计）在 `scripts/engine/harness/knowledge/api_signatures/` 分片文件。
