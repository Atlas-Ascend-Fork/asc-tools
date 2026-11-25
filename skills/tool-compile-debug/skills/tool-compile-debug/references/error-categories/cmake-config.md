# cmake_config — CMake 配置、语言模块与构建规则取证

## 诊断信号与级联消歧

涵盖 `CMake Error`（模块/包查找失败、语言未启用、参数传递错误）及构建规则阶段的 `No rule to make target` / 目标文件打开失败。先消歧**外层 CMake `message()` 转发信封**：当 `func.cmake`、`opbuild` 或自定义 `execute_process` 捕获内层 Host/Kernel 编译或链接错误后再抛 `CMake Error` 时，内层具体编译/链接诊断才是真首错，须转对应源码或 [link-error.md](link-error.md) 处理，绝不因外层报错头是 CMake 就盲改环境。

## 本地确定性取证动作

1. **审查配置入口与参数传播链**：用 `run.py inspect --project <工程根> --file CMakeLists.txt` 与 `--question build --build-dir <构建目录>` 检查配置入口 → 目标属性/宏定义 → 实际编译/链接命令的传播关系；使用 `ASC` 语言的工程核对 `find_package(ASC REQUIRED)` 与语言启用方式，采用自定义构建宏的工程保留其原有组织形式。
2. **定位 `No rule to make target` 真实断链点**：读取完整缺失目标路径与 `needed by` 链，逐项区分源文件路径/大小写拼写错误、代码生成规则未产出或未声明目标依赖、旧构建缓存残留失效路径。
3. **保留可配置性与真实退出码传播**：修复变量引用或覆盖顺序错误时保留工程原有的架构/路径配置参数，不用当前机器字面量硬编码覆盖可配置入口；同时核查顶层 `build.sh` 是否存在 `! cmake ...` 等退出码取反或管道吞错缺陷，确保全构建阶段成功返回 0、失败返回非 0。

## cannbot-knowledge 查询路由

| 未决领域问题 | 宿主加载技能 | 返回后本地核对项 |
|---|---|---|
| Ascend C 工程 CMake 语言模块、打包宏、自定义算子构建规则或跨版本配置约定 | cannbot-knowledge `knowledge-query` 技能 | 读取卡片正文，对照本机 SDK 下 CMake 模块文件的实际定义与参数签名 |
| 构建脚本涉及特定 SDK 库目标名或公开头目录变量不确定 | cannbot-knowledge `ascendc-api-knowledge-query` 技能 | 在本机活跃 SDK 目录中核实对应 CMake config 文件与导出目标 |

## 契约保护红线

严禁通过注释构建目标、跳过设备核编译阶段或缩减目标架构列表来规避配置报错；修改配置或构建规则后须在隔离副本中重跑完整配置、生成与编译流程。
