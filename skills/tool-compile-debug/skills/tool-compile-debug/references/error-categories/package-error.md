# package_error — 算子打包、元信息与描述产物闭合取证

## 诊断信号与级联消歧

常见信号为打包或安装归档阶段的 `msobjdump: ... no ... kernel`、`kernel metadata missing`、`package failed`、`.json ... not generated` 或归档脚本路径/文件名超长报错（规则见 `scripts/engine/harness/config/error_signatures.yaml`）。先做上游阶段回溯：打包阶段缺失核元信息或 `.json` 往往是因为**设备核编译被跳过、入口符号未导出或前置生成步骤未执行**，须联查 [kernel-compile-error.md](kernel-compile-error.md)、[link-error.md](link-error.md) 与 [dependency-missing.md](dependency-missing.md)。

## 本地确定性取证动作

1. **核验设备核目标是否真实编入产物**：用 `run.py inspect --question build --build-dir <构建目录>` 检查设备编译目标是否实际执行，并用本机工具链 `msobjdump` / `readelf` 检查目标文件或库中是否包含预期设备核段与导出入口符号。
2. **核查描述 `.json` 与归档脚本输入**：检查算子描述文件生成命令、输出目录变量及打包脚本读取路径是否一致；遇归档文件名或构建路径超长时，核查构建目录层级与打包参数配置。

## cannbot-knowledge 查询路由

| 未决领域问题 | 宿主加载技能 | 返回后本地核对项 |
|---|---|---|
| 自定义算子打包流程、核函数元信息生成条件、归档脚本约定或排障方法 | cannbot-knowledge `knowledge-query` 技能 | 读取卡片正文，对照本工程打包脚本、`CMakeLists.txt` 及实际中间产物目录 |
| 核函数入口修饰宏、注册宏与打包器识别契约不确定 | cannbot-knowledge `ascendc-api-knowledge-query` 技能 | 核对本机活跃 SDK 头文件中入口修饰宏与注册宏展开定义 |

## 契约保护红线

严禁通过绕过打包阶段、手工伪造残缺 `.json` 元数据或剥离必需设备核目标来制造构建返回 0 的假象；修复后须沿原配方完整产包并核实必需产物在位。
