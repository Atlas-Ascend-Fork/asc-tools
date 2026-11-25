# environment — 工具链路径、目标配置与环境一致性取证

## 诊断信号与级联消歧

常见信号为配置期找不到 `ASC` 包或工具链路径、`ASCEND_HOME_PATH` / `ASCEND_CANN_PACKAGE_PATH` 未就绪、`SOC_VERSION` 不受当前配置支持，或多套工具链头文件与库混用。注意区分两类事实：**历史失败日志记录的构建环境错误**与**当前执行机器的工具链探测状态**（`environment_checks`）——本机缺工具链仅限制重编验证，不覆盖原日志中的源码或配置根因；若编译器在编译期直接拒绝非法 `--npu-arch` 或选项值，转 [unfriendly-error-code.md](unfriendly-error-code.md)。

## 本地确定性取证动作

1. **对照日志环境与本机活跃 SDK**：运行 `run.py inspect --project <工程根> --question environment` 采集当前可见 CANN 路径与精确版本，并与原失败日志及构建脚本中的工具链引用、目标 SKU 逐项比对（不用宿主机物理 NPU 卡型替代用户指定的编译目标 SKU）。
2. **核查头库同源与 ABI 一致性**：怀疑多版本共存混链时，从原编译/链接命令提取实际 `-I`、`-L`、`--sysroot` 参数，并用 `readelf -d` / `readelf --version-info` 核对产物 `NEEDED`、`RPATH`/`RUNPATH` 及 `GLIBC_*`/`GLIBCXX_*` 符号版本需求，不靠空命令预处理猜测包含路径。
3. **单一环境收敛验证**：确认目标 SDK 安装后，在独占验证环境中仅加载该安装的 `set_env.sh` 并保持原构建选项复验，不叠加多套环境脚本。

## cannbot-knowledge 查询路由

| 未决领域问题 | 宿主加载技能 | 返回后本地核对项 |
|---|---|---|
| CANN 环境初始化规范、CMake 包发现机制、多版本隔离或目标 SKU 配置要求 | cannbot-knowledge `knowledge-query` 技能 | 读取卡片正文，核对本机实际安装目录结构、`version.cfg` 元数据与原构建配方 |
| 特定版本 SDK 组件头/库归属或目标平台支持边界不明 | cannbot-knowledge `ascendc-api-knowledge-query` 技能 | 对照本机已安装 SDK 的实际文件存在性与导出声明 |

## 契约保护红线

严禁未经用户授权擅自更换目标芯片 SKU、强制要求升级低于下限但可修的工程，或在工程配置中写死开发机私有绝对路径破坏可移植性。
