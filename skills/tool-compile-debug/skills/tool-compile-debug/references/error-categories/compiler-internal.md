# compiler_internal — 编译器内部错误（ICE）核实与最小复现取证

## 诊断信号与级联消歧

常见信号为 `internal compiler error`、`Segmentation fault`、`error in backend: Cannot select`、`Stack dump` 或前后端命令非零崩溃退出。**首要级联消歧**：检查崩溃栈之前是否先出现了非法架构/选项拒绝（如 `Unsupported NPU architecture`）或前端语法/类型首错——若存在前序错误，后续崩溃仅是连锁副产物，优先转 [unfriendly-error-code.md](unfriendly-error-code.md) 或对应前端分类修复真首错。

## 本地确定性取证动作

1. **锁定崩溃翻译单元与完整命令行**：从原始构建日志或 `run.py inspect --question build --build-dir <构建目录>` 提取触发崩溃的精确编译器可执行文件、完整参数、目标架构与输入源文件。
2. **隔离副本复现与预处理分界**：仅在源码树外的独占临时目录复现同一编译命令，区分预处理阶段问题与后端指令选择/优化 pass 崩溃；需要局部等价改写规避时（如拆分极端展开或改写触发特定后端缺陷的局部表达式，参看 [compiler-limit.md](compiler-limit.md)），每步验证原配方编译结果。
3. **保留可复现证据包**：确属工具链内部缺陷且无法在不违背契约下等价规避时，在独占目录保留最小复现用例、完整命令与工具链版本信息，按交接协议整理上报材料。

## cannbot-knowledge 查询路由

| 未决领域问题 | 宿主加载技能 | 返回后本地核对项 |
|---|---|---|
| 已知编译器后端限制、指令选择报错模式或等价源码规避经验 | cannbot-knowledge `knowledge-query` 技能 | 核对卡片对应的 CANN/编译器版本、目标架构及触发代码结构是否与现场一致 |
| 触发崩溃的特定内建函数或设备 API 在目标架构上的合法调用约束 | cannbot-knowledge `ascendc-api-knowledge-query` 技能 | 核实本机 SDK 头声明与参数范围，排除非法入参引发的后端断言崩溃 |

## 契约保护红线

缩减复现件仅作定位与上报证据，严禁把删减计算逻辑后的最小复现件写回原工程冒充修复，也不把修改原优化/目标配方后的编译结果谎报为原配方通过。
