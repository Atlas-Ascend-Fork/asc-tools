# api_incompatible — AscendC API 不兼容

## 根因

当前编译上下文中的调用与声明不匹配。常见候选如下；分类和索引未命中都不证明API不存在。普通对象、标量或SDK归属未证实的裸函数出现未声明错误时，可保持`unknown`；先查真实使用点和声明，不能按标识符名字把局部源码问题升级成SDK版本不兼容。

1. **命名空间/拼写错**：`did you mean`或大小写差异可提供候选；例如`Softmax`与`SoftMax`不同，
仍须核完整namespace、调用层及实参。
2. **跨版本改名/废弃**:符号在旧版本叫 A、新版本改叫 B 或被 deprecated。编译器只报
`no member named` / `use of undeclared`,**不会**说"它改名了"。→ 查 [`../knowledge/api-version-drift.md`](../knowledge/api-version-drift.md)。
3. **入口/目标条件**：声明可能因聚合头未导出或目标条件不可见；两者要分开核验，
不能从一个入口缺失推出整个API不支持。见[目标门控](../knowledge/soc-arch-gating.md)。
4. **不适用或存在性未知**：也可能用了错误API层、工程包装缺失，或当前版本没有该声明。
按[存在性排查](../knowledge/hallucinated-api.md)补证；没有适用声明时保持未知，不直接重写算法。

## 典型报错（真实日志样貌）

```
add.asc:38:43: error: use of undeclared identifier 'AscendCC'; did you mean 'AscendC'?
```
（提示一个拼写候选；当前定义和调用意图吻合时可局部修正。）

```
error: no member named 'Softmax' in namespace 'AscendC'
error: use of undeclared identifier 'SetCcTiling'
```

## 修法（按子类）

| 子类 | 判别 | 修法 |
|---|---|---|
| 拼写/casing | 候选声明、类型和调用意图一致 | 局部改名，用当前工程验证 |
| 改名/废弃 | 版本来源及当前声明支持映射 | 对照旧/新参数与语义，不能只换短名 |
| 入口/arch门控 | 准确9.0头、条件宏与实际编译目标支持判断 | 修正错误入口或串用的环境；用户目标正确时不能换SoC逃避问题 |
| 存在性仍未知 | 索引或一次搜索没有结果 | 保留未知，补最能区分候选的声明/实例证据；算法替代须保留功能和数值契约 |

先读已有诊断的适用性与来源证据，证据充分时直接局部修改；出现源码或编译反证时撤销该假设。不按固定检索顺序、近名分数或“确定性”字样越过条件核验，也不要求简单错误做全套排查。

## 说明

调用 CANNBOT `ascendc-docs-search` 技能来查目标版本的 API 声明与来源；具体目标可用性仍须当前头与编译证据。检索无结果或文档覆盖限制不构成“不存在”的证据。



## 本机核实命令

```bash
# 查 API 是否在当前安装中声明
grep -rn "API名" "$ASCEND_HOME_PATH/latest/ascendc/include" --include="*.h" | head -10

# 查该声明的目标宏条件（#if __NPU_ARCH__）
grep -B3 -A3 "__NPU_ARCH__" "$ASCEND_HOME_PATH/latest/ascendc/include/<头文件>.h"

# 核当前编译目标
grep -n "SOC_VERSION\|ARCHITECTURES" CMakeLists.txt
```

调用 CANNBOT `ascendc-docs-search` 技能来查完整声明与兼容性文档。
