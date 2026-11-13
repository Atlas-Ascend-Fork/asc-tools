# asc-tools 接入说明

来源为 [Aprof](https://github.com/jieran-zhang/Aprof) 的
`refactor/skill-libs-scaffolding` 分支，固定提交
`d7c6bf69ff31403569ea91b64441ded1fd334144`。
仅导入 `skill-libs/graph-base/1.3.2/`，映射为本目录
`skills/tool-ops-optimize/`；没有导入 1.3.0、1.3.1 或仓库 scaffolding。

版本为 graph-base 1.3.2，runtime 0.8.3。它沿用 D7/P4/O7/routing6
知识身份，将开发报告、纸面样例和部分旧构建材料留在来源仓库。
包仍未封版，不切换默认版本。开发材料入口见
[归档索引](workflows/references/development-records.md)。

## 导入范围

保留运行时、技能、角色、契约、知识、来源及许可证。
按本 PR 既有范围省略 96 份 `sources/development/evaluation/` 下的
压缩开发历史；每份原路径及 SHA-256 记录在
[导入清单](tools/frozen-files.json) 的 `omitted_history_artifacts`。
原文件可从上述固定 GitHub 提交获取，省略不修改其他文件的原始哈希。
这些材料的完整历史重放需使用来源仓库。

接入适配包括宿主发现用顶层 `SKILL.md`、正确的插件网址、CANN 2.0
许可证及活跃代码的版权头和格式。版权人为 AProf contributors；
第三方来源和许可证保留原声明。测试夹具显式保留 pytest 注册，
补充 NumPy/pytest-xdist 测试依赖，并修正旧目录和主机架构假设。
真实 aarch64 工具链限制和验证断言保持。

## 冻结身份和仓库检查

版本化知识的证据原文、历史迁移、schema、`_diagnosis_v1.py` 及
`_knowledge_v1.py` 按源文件字节保留。`knowledge.py` 是 1.3.2 新增的
活跃报告管理入口，正常执行代码检查；冻结编译器位于 `_knowledge_v1.py`。
`diagnosis.py` 为活跃的诊断入口，负责来源归档的逻辑路径兼容。
39 份来源头文件以原始字节的文本归档保存，详见[来源归档说明](references/source-archives.md)。

`.gitattributes` 防止自动换行转换。格式、拼写及版权改写只对明确的
冻结范围例外；JSON、冲突标记、私钥检查及逐文件 SHA-256、集合和入口检查仍执行。
活跃代码保持原门禁。Markdown 过滤步骤仅在冻结校验通过后移除清单内历史文档；
已适配的阅读文档、活跃文档和其他技能继续检查。评论触发工作流若来自目标分支，首次生效需维护者确认。

## 独立使用和测试边界

保留完整本目录，运行 `tools/install.py --allow-unsealed --runtime --skills-dir <宿主目录>`。
安装不限定 Cursor，也不要求原 Aprof 工作树。Linux 下的独立结构检查：

```bash
python3 tools/check_import.py
python3 tools/release.py --allow-unsealed
tools/aprofctl diagnosis verify --root .
```

完整开发回归需要来源仓库的 1.3.1/1.3.2 开发归档，见其索引；这些归档
不是独立安装依赖。独立包的结构、知识校验、离线搬迁回放和安装检查分别运行。
测试、历史实验摘录与未验证范围见 [验证说明](VALIDATION.md)。

## 文档引用修复来源

本次同时接入来源仓库 [PR #8](https://github.com/jieran-zhang/Aprof/pull/8)
的已验证修复，提交为 `9e5adf9d62c39053978f1bb93be4deb612077b3c`。
该修复分支以 `refactor/skill-libs-scaffolding` 为目标；基础包来源仍为上述固定提交。
136 份 Markdown 阅读文档与 112 份去重后的证据原文分别保存、分别校验。
`tools/documentation-adaptations.json` 记录每处改动及两份内容的 SHA-256；
证据读取继续使用原始字节，现有图、知识快照和冻结生成器不改写。
包内 `check_import.py` 同时检查阅读文档和原文的身份，不能以阅读修改替换证据。
82 条失效图片引用已从阅读文档移除，图片文件未补入；处理前的目录和路径清单保留在来源 PR 中。
