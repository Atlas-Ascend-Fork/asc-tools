# API 存在性与调用层排查

`no member named`只说明当前编译上下文未找到该成员；可能是拼写、namespace、API 层、入口、版本或目标条件不同。未命中索引也可能只是覆盖不足，不能判定符号不存在。

先保留编译器给出的完整候选和模板链，再核实际调用：

1. 区分工程自有类型/包装与 vendor API；查定义和 include/生成链。
2. 对照完整 namespace、实参类型、模板参数与调用层。Tensor 的 `AscendC::Exp`、
Reg 的 `AscendC::Reg::Exp`、SIMT 的标量接口不能互相按短名替换。
3. 查准确安装版本中的声明头及其条件。聚合头未导出时，再核逐头入口；存在声明仍需实例探针。
4. 仅在当前错误和契约支持局部改名时修改，再用原工程构建配方验证。

CANN 9.0.0（build `20260428_134817545`）已核声明包含 `AscendC::Silu`、 `AscendC::SoftMax`，以及不同层的 Exp。查其他声明的路径与行号时，调用 CANNBOT `ascendc-docs-search` 技能来检索目标版本文档（取证字段完整的结构化记录在 `scripts/engine/harness/knowledge/api_signatures/` 分片文件供引擎消费）；这些记录没有自动证明任意实参或目标运行支持。 `MicroAPI = Reg`别名也有目标宏条件，不能从别名文本推断任意目标可见。

`Softmax`与`SoftMax`大小写不同，可以形成拼写候选；不能据此要求在950重写 SoftMax。当前索引有 `ReduceMean` 声明，不能沿用“只能叫 Mean”的旧结论。若检索仍无适用声明，报告存在性未知，并围绕缺失证据设计最小探针。

需要改变算法时先固定独立数学和数值契约，再读[实现参考](operator-idiom.md)。不能通过删除调用、改变精度或修改 golden 把未声明错误变成表面通过。



## 决策流程（诊断输出 → 本页流程映射）

诊断输出的 `kb_evidence[].evidence` 三态映射到本页 4 步流程：

| `kb_evidence.evidence` | 含义 | 对应本页步骤 |
|---|---|---|
| `kb_affix_exact` | 前后缀精确命中已知声明 | 步骤 2（对照 namespace/类型/调用层），使用附带 signature |
| `kb_near_name` | 近似匹配（difflib/前后缀） | 步骤 1（先区分自有类型 vs vendor API），再核步骤 2 |
| `api_version_map` | 版本迁移记录命中 | 读 [版本映射候选](api-version-drift.md) |
| （空/无命中） | 知识库无此符号 | 不判「不存在」，走步骤 3（查准确安装版本）+ docs-search 查证 |

## 本机核实命令

```bash
# 在当前 CANN 安装中搜索符号
grep -rn "符号名" "$ASCEND_HOME_PATH/latest/ascendc/include" --include="*.h" | head -10

# 区分大小写变体
grep -rn "符号名\|SYMBOL" "$ASCEND_HOME_PATH/latest/ascendc/include" --include="*.h" -i | head

# 查完整声明（含模板参数和默认值）
grep -A5 "符号名" "$ASCEND_HOME_PATH/latest/ascendc/include/<头文件>.h"
```
