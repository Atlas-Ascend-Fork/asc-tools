# lint 工具规则源 — detector 机械校验

本清单由模型读取知识库后逐项核对。文中的集合、正则与计数表达式描述判定标准，不要求执行额外程序。结论须附文档依据；无法确认的项目标为“未验证”，不能默认通过。

## 基线说明

- **runtime 基线**：`references/detectors/runtime/` 子目录共 **81 ID**（数量校验基线）
- **static_preventive 基线**：`references/detectors/static_preventive/` 共 10 篇，走 `nature` 字段校验（见校验项 11）
- ID 基数动态取自 `references/indices/seed_mapping.md` 映射表行数，不硬编码

## 校验项 1：detectors 数量

- **规则**：`count(references/detectors/runtime/*/*.md) == seed_mapping.md 映射表数据行数`
- **失败处置**：报错并列出缺失/多余 ID

## 校验项 2：detectors ID 集合一致性

- **规则**：`references/detectors/runtime/*/*.md` 文件名集合（去 `.md`）== `seed_mapping.md` ID 列全集
- **机械表达式**：

  ```text
  FILENAME_SET   = 读取 references/detectors/runtime/*/*.md 文件名，去掉 .md 后缀所得集合
  SEEDMAP_ID_SET = 读 references/indices/seed_mapping.md "ID" 列全集
  FILENAME_SET == SEEDMAP_ID_SET
  ```

- **失败处置**：报错并 diff 两集合

## 校验项 3：文件名命名约定

- **规则**：正则 `^[a-z][a-z0-9_]*\.md$`
- **失败处置**：报错列出违规文件名

## 校验项 4：seed_mapping 类别归属一致性

- **规则**：每个 ID 的"类别目录"字段非空且为 9 类单一 slug 之一（memory_overflow / sync_hang / datacopy / loaddata / cube / fixpipe / scalar_vec_compute / alignment_config / system_bus）
- 同时检查实际文件所在目录与映射目录一致；仅 slug 合法但文件放错目录也报错。
- **失败处置**：报错列出空值或非规范 slug

## 校验项 5：seed_mapping 覆盖完备性

- **规则**：每个 ID 在 seed_mapping 中恰好出现 1 次；`cross_references.md` 引用的异常编号须存在于该映射
- **失败处置**：报错列出未归属/多重归属

## 校验项 6：错误码索引完备性

- **规则**：`error_code_index.md` 覆盖全部 ID（有码 + 无码合计 == seed_mapping ID 全集）；错误码映射与各 detector 的错误码字段一致
- **机械表达式**：

  ```text
  有码ID ∪ 无码ID == SEEDMAP_ID_SET（无遗漏、无重叠）
  ```

- **失败处置**：报错列出缺口

## 校验项 7：关键词索引完备性

- **规则**：`keyword_index.md` 覆盖全部 ID
- **机械表达式**：

  ```text
  KEYWORD_ID_COVER = ⋃ 读 references/indices/keyword_index.md "异常 ID 列表" 列展开
  SEEDMAP_ID_SET ⊆ KEYWORD_ID_COVER
  ```

- **失败处置**：报错列出未被任何关键词覆盖的 ID

## 校验项 8：模板符合性（必填段落 + 四标记 + 执行指引三字段）

- **规则**：按 `references/templates/detector_template.md` 的字段定义校验 runtime detector。必填章节、四标记、合法匹配模式及执行指引三字段的定义以该模板为准；受限判定和计分口径见 `SKILL.md` Step 2。
- **机械表达式**：

  ```text
  REQUIRED_SECTIONS = detector_template.md「八大必填段落」的章节名
  RULE_MARKERS = detector_template.md「四标记字段」的标记名
  MATCH_MODES = detector_template.md「匹配模式说明」表的模式名
  GUIDE_FIELDS = detector_template.md「检测执行指引」的三个必填字段名
  对每个 references/detectors/runtime/{category}/{ID}.md：
    含全部 REQUIRED_SECTIONS
    基本信息表的异常编号 == 文件名去 .md；硬件事件编号不得代替正式 ID
    类别 == 实际父目录；kernel_diff_type == seed_mapping 对应枚举
    每条 RULE 块含全部 RULE_MARKERS，且【匹配模式】值 ∈ MATCH_MODES
    完整规则 ID 在所属文档内唯一，前缀与文件正式 ID 一致
    「检测执行指引」段含 GUIDE_FIELDS，字段正文非空（允许加粗、行内正文或缩进子列表）
  ```

- **失败处置**：报错列出违规文件 + 缺失段落/标记/执行指引子项的具体位置

## 校验项 9：无占位文档

- **规则**：全部 `references/detectors/runtime/{category}/{ID}.md` 不含"待补全"字样
- **机械表达式**：

  ```text
  全部 runtime detector 中包含“待补全”字样的文件集合为空集
  ```

- **失败处置**：报错列出含占位标记的文件

## 校验项 10：frontmatter / YAML 可解析性

- **规则**：`SKILL.md` frontmatter 含非空 `name`、`description` 字段；由模型核对含 frontmatter 的 .md 的分隔符、缩进、引号和键值结构是否符合 YAML 语法。
- **结论边界**：模型未发现结构问题不等于实际解析器已验证通过；复杂结构或歧义无法确认时标记“未验证”，不得声称调用了解析器。
- **失败处置**：列出发现的语法问题、缺失必填字段或无法确认的位置与原因。

## 校验项 11：static_preventive nature 字段校验

- **适用范围**：`references/detectors/static_preventive/*.md`（10 篇，独立基线，不计入 runtime 81 基线）
- **规则**：每篇含 `性质` 字段，取值 ∈ `{预防型, 信任型, 索引型, 探索型}`；按性质约束内容：
  - **预防型**（5 篇）：必含 `## 检测规则` + `## 修复方案` + 至少 1 条 code_block
  - **信任型**（1 篇）：不含 code_blocks（仅说明性输出）
  - **索引型**（2 篇）：不扫源码（无 RULE 匹配段），含 EventID 索引表
  - **探索型**（2 篇）：标注"未确认"，含候选检测方向
- **失败处置**：报错列出 nature 字段缺失/取值非法 + 性质-内容约束违反

## 校验项 12：规则来源分布统计

- **规则**：lint 报告中输出"规则来源分布"统计项，按来源（LLM 推断 / 代码 diff 直接提取 / API 文档约束 / 运行验证）统计各 detector RULE 的来源占比
- **统计项**：仅透明化输出，不作为阻断条件
- **目的**：展示当前规则来源分布；按实际文件统计，不固定写死比例
- **范围与分母**：统计当前执行范围内 runtime detector 的 RULE 块，每条一次；增量模式不冒充全库统计。未执行统计时明确注明。static_preventive 如需统计，单独列出，不能混入 runtime 分母。
- **未标注来源**：缺少来源字段时计入“未标注”，不推断为 LLM 或已验证；仍计入总数分母。
- **复合来源**：`LLM 推断-已验证` 的来源归入“LLM 推断”，另列“验证状态=已验证”；该标签不自动证明原始目标异常已验证，须保留 OOB 兜底与目标的区分。
- **未知来源**：其他无法归一的值计入“其他/未识别”并列明原文；各来源数量之和必须等于 RULE 总数，比例统一以该总数计算。
