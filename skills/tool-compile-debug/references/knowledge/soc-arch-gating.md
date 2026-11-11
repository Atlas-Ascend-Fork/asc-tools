# 头入口、目标条件与 API 支持

头文件中的 `#if`可以限制某个入口或某组声明。它不能单独证明所有同名 API、其它入口或另一调用层在目标上不可用。诊断的 `header_visibility`是核验线索， `applicability=unknown`不得升级为架构禁用结论。

当前目标定位中，910B 系对应2201、950系对应3510。910_93 系列历史上曾误归 3003、现归入 2201；准确 SKU 仍需核当前安装的 platform 配置和真实编译参数。宿主有910 NPU不等于所有编译都针对2201，也不证明本机能完成3510交叉编译。

对缺声明或疑似目标问题，固定同一份最小调用与依赖：

| 证据 | 能回答什么 |
|---|---|
| namespace、层、完整声明、实参/模板类型 | 是否在比较同一个接口与调用 |
| 准确 CANN 版本/build、声明头 SHA256与行号 | 声明来自哪次安装 |
| 聚合入口与逐头入口、原始宏条件 | 声明从哪些路径进入当前TU |
| 后端可用正控与真实编译 argv/对象 | 当前实例是否在该目标编译 |
| 真机合法输入及独立 golden | 当前实现的运行和数值行为 |

CANN 9.0.0（build `20260428_134817545`）已核声明包含 `adv_api/activation/silu.h`、`adv_api/activation/softmax.h`等完整声明。旧聚合扫描不能推出“SoftMax/Silu/Exp 在3510必须分解”。新旧入口的声明、模板实例化、device link和运行应分别报告。

若任务目标已明确，沿既定目标核工具链和工程配置；只有目标意图未定且选择会改变功能时才确认。确需迁移时读[2201/3510 迁移核验](arch-migration-2201-3510.md)；不要用无关target或删除语义调用制造成功。

随包引擎 `scripts/engine/harness/knowledge/soc_feature_matrix.yaml` 中的入口记录按 `header_visibility` 消费，不作为完整支持矩阵。FP8/MX 类型的目标能力裁决见 [FP8/MX 架构门控](fp8-mx-arch-gating.md)。
