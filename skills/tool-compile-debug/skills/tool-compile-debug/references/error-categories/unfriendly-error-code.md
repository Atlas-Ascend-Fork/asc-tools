# unfriendly_error_code — 非法目标架构或编译选项拒绝取证

## 诊断信号与级联消歧

常见信号为编译器直接拒绝传入的目标架构或命令行选项（如 `Unsupported NPU architecture`、`no aicore arch specified`、`Cannot find option named '...'`），且其后常伴随 `LLVM ERROR` 或 `Stack dump` 等级联崩溃日志。注意两点消歧：其一，紧随其后的崩溃栈是选项被拒的后果，修复非法选项即可消除；其二，与 [environment.md](environment.md) 区分——`environment` 多为工具链路径未配置，本类则是参数已传入但取值与当前编译器/目标不兼容。

## 本地确定性取证动作

1. **提取实际编译命令与配置源**：用 `run.py inspect --question build --build-dir <构建目录>` 及 `CMakeLists.txt` / 构建脚本，追踪被拒的 `--npu-arch`、`SOC_VERSION`、`CMAKE_ASC_ARCHITECTURES` 或自定义编译标志在何处拼装生成。
2. **核对本机活跃编译器版本与选项支持**：用 `run.py inspect --question environment` 确认当前 CANN 版本与编译器路径，核对工程传入的目标型号或选项名是否拼写错误、混用了架构内部代号与公开 SKU 名，或沿用了当前编译器版本不支持的旧标志。
3. **修正配置入口后原配方复验**：在工程配置入口对齐用户要求的目标芯片 SKU 与合法编译标志，不忽略后续是否仍有真实源码错误。

## cannbot-knowledge 查询路由

| 未决领域问题 | 宿主加载技能 | 返回后本地核对项 |
|---|---|---|
| 目标芯片 SKU、CMake 架构配置项与编译器 `--npu-arch` 参数对应关系或选项演进 | cannbot-knowledge `knowledge-query` 技能 | 读取卡片正文，对照本机活跃 SDK 下 CMake 架构映射模块与编译器帮助信息 |
| 目标芯片型号在当前 SDK 下的编译期宏定义与公开头支持边界 | cannbot-knowledge `ascendc-api-knowledge-query` 技能 | 核对本机活跃 SDK 头文件与构建模块实际接受的参数值 |

## 契约保护红线

严禁为绕过架构参数报错而擅自将用户指定的目标芯片（如 `Ascend950` 或特定 `Ascend910` 子型号）篡改为其他型号，详见[知识协作契约](../cannbot-contract.md)。
