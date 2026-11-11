# compiler_internal — 疑似编译器内部错误

## 根因

内部错误、段错误或栈转储是排查编译器异常的线索，先确认来自本次编译器进程及完整日志。崩溃不排除源码构造、非法参数、资源或工具链环境触发，也不证明问题发生在设备后端。源码里的CCE字样或手写入口名不一致不属于崩溃证据；普通未声明错误先核调用和声明。工具可以整理证据与最小复现；是否属于编译器缺陷、采用何种规避，仍需实际复现和反证。

## 典型报错（真实日志样貌）

```
internal compiler error: ...
Segmentation fault
LLVM ERROR: Cannot select: ...
Stack dump:
0.  Program arguments: ...
Fatal Err: A fatal error occurred in iternel process ...
```
这些消息可触发现有`compiler_internal`分类；兼容`phase=kernel_compile`可能来自类别推断，不单独证明Host/Device源码区域或真实内部pass。先核实际命令、最早具体错误及进程退出状态。

> 注意：有些工程里崩溃是**后果**不是根因——比如 bisheng 先拒绝一个非法 arch 选项
> (`unfriendly_error_code`),后续才 `LLVM ERROR`/`Stack dump`。诊断引擎把真错误排在 ICE artifact 之前
> 捕获。先看有没有更早的、更具体的错误行。

## 修法（产证据，不是"修编译器")

1. **缩最小复现**：在源码树外的独占副本里手工二分缩减，冻结真实编译命令与源文件，
保留原输入和完整构建配方；每次确认仍是同一触发条件和崩溃签名。缩减物只作复现证据，不直接作为算子修复交付。
2. **找规避**:常由某个具体构造触发（巨型展开、特定 intrinsic 组合）——改写该构造往往能绕开。
可配合`compiler_limit`的候选手段；每次记录变更，只用实际结果判断。降优化改变了原配方，编过只能证明该规避的编译结果，仍须核功能、原数值判据及适用的性能要求。
3. **上报**:保留最小复现 + 编译命令 + 编译器版本，提交昇腾团队。工具不声称能修复编译器自身。

## 说明

分类匹配规则与建议动作由随包引擎驱动（`scripts/engine/harness/knowledge/error_signatures.yaml` 的 `internal_compiler_error` 规则）。本页示例形态仅用于识别，不构成对编译器缺陷的判定。
