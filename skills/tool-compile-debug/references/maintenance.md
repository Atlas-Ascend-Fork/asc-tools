# 源码维护与发布检查

本页供维护技能源码时使用。日常编译诊断不需要更新知识库或重新生成清单。

## 独立维护范围

包含 `SKILL.md` 的技能目录是完整维护单元。独立仓库将它作为仓库根目录；在 asc-tools 源码仓中，它对应 `skills/tool-compile-debug/`：

| 内容 | 修改位置与验证 |
|---|---|
| 技能触发与修复流程 | `SKILL.md`、`references/`；同步相关正反触发和回答评测 |
| 命令入口和维护工具 | `scripts/`；执行随包测试 |
| 诊断、定位、验证、脱敏 | `scripts/engine/harness/engine/`；补代表性输入与结果断言 |
| 构建辅助 | `scripts/engine/harness/runtime/`；核对命令参数和原构建配方是否保留 |
| 知识与版本边界 | `scripts/engine/harness/knowledge/`；保留来源、适用版本和目标限制 |
| API 分片清单 | `scripts/engine/harness/knowledge/api_signatures/manifest.json`；由包内维护命令更新 |

这些 Python、shell 和 YAML 文件均随包提供源码，维护不需要下载另一个交付包或开发仓。
修改发生在源码工作区，再由宿主安装器分发整个目录；已安装目录在运行时保持只读。
技能版本的唯一来源为 `SKILL.md` 中的字符串 `metadata.version`。补丁修复递增末位；版本号与实际分发文件指纹共同标识发布内容。`face_selfcheck.py --json` 返回 `skill_version` 和逐文件指纹，非法或缺失版本会使自检失败。版本号不代表某个 CANN/SoC 组合已经验收。
在独立仓库根运行自检时，根目录的 `.git` 元数据不计入交付文件与指纹；其他包内文件仍接受完整性检查。

如果在使用生成器制作此包的开发仓中工作，仍应修改该仓的真源，再由生成器同步；
不要把这里的独立维护方式用于覆盖开发仓的生成规则。
在独立仓库或宿主源码仓中继续维护时，以该仓当前版本为修改基线，合并上游更新前先比较差异，保留已合入的修复。

文档和评测说明提到外部协作技能时，将来源与精确名一起写明，例如“调用 CANNBOT `ascendc-docs-search` 技能核对目标版本声明”。每个技能名分别标注 CANNBOT，并说明宿主可用时才按需调用。评测配置中的技能 ID 和匹配模式保留原值，不把来源词加进机器标识。

检查引用时按完整名称及上下文区分技能与命令选项：`--npu-arch=dav-2201` 是编译器选项，CANNBOT `npu-arch` 技能用于核对架构事实。选项名、代码标识和文件路径保留原值，不能仅凭包含技能名的子串将其改写成技能调用。

## 更新 API 知识分片

1. 从目标版本 SDK 声明或可追溯资料确认修改，记录版本、SKU 和适用前提。
   先检查原清单并保留修改前的源码差异基线：

   ```bash
   python3 "$SKILL_DIR/scripts/update_kb_manifest.py" --check
   ```

2. 修改 `api_signatures/part-*.yaml` 中的条目，保留字段类型、条目顺序及引文语义。
   每片不超过 450 KiB；需要拆片时只在完整条目之间拆分，按连续的四位编号排列。
   `\xNN` 等已有 YAML 转义是字符串表示，不应按表面拼写改写 SDK 符号。

3. 根据预期变更独立确定分片数与总条目数，再生成清单。以下变量是维护者核对后的数量，
   不能从损坏目录中现数现填来绕过检查：

   ```bash
   python3 "$SKILL_DIR/scripts/update_kb_manifest.py" --write \
     --expected-parts "$EXPECTED_PARTS" --expected-entries "$EXPECTED_ENTRIES"
   python3 "$SKILL_DIR/scripts/update_kb_manifest.py" --check
   ```

   命令只更新 `manifest.json`，不重写 YAML。每条记录须保留字符串字段 `name`、`signature`、
   `header`、`kind`、`api_level`、`category`、`brief`，只有 `brief` 可以为空。
   缺片、额外文件、数量不符、重复映射键、必需字段缺失或类型错误、超大分片及非 LF 行尾会导致失败；失败前不会替换原清单。
   清单说明完整性，不证明知识正确。收到不完整安装包时应恢复完整包，不用维护命令接受缺失数据。

4. 核对代码差异、条目语义与原资料，运行下述检查。API 声明中的真实名字不能为了通过拼写检查而改名；
   如需 YAML 转义，确认解析值未变。新增 API 或版本不能仅凭清单通过就宣称已完成真实编译验证。

## 包内回归与发布证据

在 Linux 源码工作区，设 `SKILL_DIR` 为此技能目录的绝对路径；需要 Python ≥3.9 和 PyYAML：

```bash
python3 -m unittest discover -s "$SKILL_DIR/tests" -v
python3 -m unittest discover -s "$SKILL_DIR/tests" -v
python3 -m pytest "$SKILL_DIR/tests" -q
python3 -m pytest "$SKILL_DIR/tests" -q
```

以上测试在独立可写副本运行，另需 pytest；移除 `PYTHONDONTWRITEBYTECODE` 和 `PYTHONPYCACHEPREFIX`，不要传 `-B`，重复之间不清缓存。测试对干净的临时包副本执行成功自检和故障注入，不把测试运行器的缓存带入被测包；真实字节码污染仍须拒绝。

下面的安装检查对另一份未运行测试的干净安装包执行，设 `INSTALLED_SKILL_DIR` 为其绝对路径。测试目录中的缓存会使正式安装自检失败，不能因此放宽发布包清洁要求。

```bash
python3 "$INSTALLED_SKILL_DIR/scripts/update_kb_manifest.py" --check
python3 "$INSTALLED_SKILL_DIR/scripts/face_selfcheck.py" --json
python3 "$INSTALLED_SKILL_DIR/scripts/preflight.py" --json
```

包内测试使用临时目录，不依赖父仓测试、其他技能、网络或真实 CANN 编译。
诊断行为测试运行实际引擎；验证状态测试控制构建返回和证据，不能替代真实工具链编译。
真实编译修复、宿主触发与安装分发分别验收，按[触发评测说明](trigger-evaluation.md)区分材料校验与行为结果。

在宿主仓执行其既有 pre-commit 和适用 CI，确认检查实际扫描了新增文件，钩子没有留下未提交改写。
宿主是否自动发现包内 unittest 取决于该仓 CI；未接入时须单独运行并提供日志，不能宣称已有仓级自动守护。
知识、脚本或技能描述发生变化后，以最终提交重新核对相应证据，不沿用旧包的通过结论。
