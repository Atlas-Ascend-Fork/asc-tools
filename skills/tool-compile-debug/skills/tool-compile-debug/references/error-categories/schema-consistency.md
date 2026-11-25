# schema_consistency — 算子原型定义、生成接口与消费映射取证

## 诊断信号与级联消歧

涵盖算子原型描述（`.json` 或 Host `OpDef` 注册）、代码生成产物与 Host/Kernel 参数消费约定之间的错配，典型线索如 `prototype ... schema`、`input_desc missing`、`host_kernel arg` 不一致或 `generated code is stale`。区分两类问题：原型/注册接口层失配用本页；纯 Tiling 数据结构字段失配转 [tiling-consistency.md](tiling-consistency.md)。注意不同层的输入、输出、`workspace` 与 `tiling` 形参按工程生成规则映射，不要求各层参数字面同名或等长。

## 本地确定性取证动作

1. **定位权威原型真源与生成链**：用 `run.py inspect --project <工程根> --file <原型/注册/核文件>` 与 `--question build --build-dir <构建目录>` 查清本工程以 `.json`、Host 注册代码还是模板为生成真源，逐一对照输入/输出顺序、数据类型、属性与参数传入位置。
2. **同源错配成组闭环**：若头文件名、结构体成员与入口形参源自同一模板或原型定义，同时核清生成真源与全部受影响消费点并一次对齐，避免只改一处引发多轮串行报错。
3. **保护手写实现更新陈旧生成物**：确认生成产物过期时，按原工程生成规则在隔离目录比对并仅更新生成边界内的产物，严禁整仓重生成覆盖手写核函数与 Tiling 逻辑。

## cannbot-knowledge 查询路由

| 未决领域问题 | 宿主加载技能 | 返回后本地核对项 |
|---|---|---|
| 算子原型定义字段规范、`OpDef` 注册宏与核函数入口参数映射规则 | cannbot-knowledge `knowledge-query` 技能 | 读取卡片正文，核对本机活跃 SDK 注册头文件与工程生成脚本规则 |
| `OpDef`、`OpParamDef` 等注册类成员接口或数据类型枚举签名不明 | cannbot-knowledge `ascendc-api-knowledge-query` 技能 | 在本机活跃 SDK 的 `register/` 头文件中核实精确方法签名 |

## 契约保护红线

保持任务规定的外部算子名称、输入/输出张量契约、支持的数据类型与实现路径；不靠删减必选参数、降级支持类型集合或偷换算子原型来消除校验报错。
