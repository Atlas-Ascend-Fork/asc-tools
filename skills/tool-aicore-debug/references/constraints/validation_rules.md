# lint 执行范围

维护者显式调用 `lint` 时使用本文件；输入参数及默认值统一见 `SKILL.md` 的「lint 参数」。校验定义、编号、基线及失败处置统一见 `references/constraints/mechanical_checks.md`。

各模式均由模型读取相应文档并执行核对，不依赖额外检查脚本或第三方解析库。检查清单中的表达式是判定标准，不是必须执行的程序。

## 模式选择

| validation_mode | 文件范围 | 执行检查 |
|-----------------|----------|----------|
| full | 全部 runtime detector、static_preventive 文档及相关索引、模板、技能元数据 | 执行 mechanical_checks.md 全部校验项 |
| incremental | changed_seeds 对应的 runtime detector，及受变更影响的索引 | 执行校验项 3、8、10、12（仅本次范围的非阻断来源统计）；涉及错误码或关键词时追加 6、7，并以完整映射表检查覆盖，不将未变更 ID 当作缺失 |
| regression | 全部 runtime detector、static_preventive 文档及相关索引、模板、技能元数据 | 重新执行 mechanical_checks.md 全部校验项 |

## 输出

按 `references/constraints/mechanical_checks.md` 的编号顺序输出检查结果；增量模式注明文件范围和未执行的检查。规则来源统计的非阻断性质以校验项 12 为准。输出 lint 报告，不套用 diagnose 的修复报告模板。

逐项注明“通过 / 不通过 / 未验证 / 不适用”，并附核对依据；来源分布标为“统计”。未读取完整范围、无法确认 YAML 结构或无法完成统计时明确标注限制，不将模型推断写成程序解析、编译或运行验证结果。
