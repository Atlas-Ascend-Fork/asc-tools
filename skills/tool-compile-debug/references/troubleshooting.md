# 安装与运行排障

先运行 `python3 "$SKILL_DIR/scripts/preflight.py" --json`，必要时用 `face_selfcheck.py --json` 获取必需路径问题与文件SHA256清单。不要从重复调用或业务源码改动解决工具安装故障。

| 现象 | 检查与处置 |
|---|---|
| 退出码2，Unsupported platform | 使用Linux及Linux Python。原生Windows Python或Git Bash执行入口时会提示支持范围；包内没有PowerShell脚本。用WSL时明确选择具有Python和Bash的Linux发行版 |
| no usable Python / 指定解释器不可运行 | 把 `ASCENDC_DOCTOR_PYTHON` 指向可运行的Linux Python 3.9+；完整引用含空格路径 |
| 缺PyYAML | 用自有venv装PyYAML，并用该解释器跑预检；可逆脱敏另需cryptography |
| bundled engine not found / missing required file / empty required file | 重新安装完整发布目录；空 `__init__.py` 合法，其他必需文件不可空 |
| KnowledgeBaseIntegrityError / API signature manifest 或 checksum 错误 | 分片知识库不完整或已被改动；重新取得完整包，不要删分片、跳过校验或手工重写 manifest 掩盖问题。安装修复前仍可由调用方依据本地源码和 SDK 继续取证 |
| engine import失败或PROJECT_ROOT解析到其他目录 | 用新Python进程运行随包入口，清理误传的PYTHONPATH/宿主模块污染，不依赖开发仓库 |
| output path inside skill pack | 把主输出和 `--stats` / `--meta-out` 一起移到安装目录外；符号链接也按真实目标检查 |
| PermissionError | 输出目录需可写；不要给安装目录加写权限绕过保护 |
| runtime artifacts inside installed skill | 保留证据后重新取得干净发布目录；构建临时目录必须在源码树外独占 |
| JSON缺失/损坏 | 保留退出码、stderr与完整stdout，先判断解析/路径/依赖/启动故障；不要把缺字段当未命中 |
| diagnose JSON中有error | 诊断工具自身失败，不是未知编译类别；按error.type与路径信息排查 |
| OpenCode加载skill或搜索时下载rg失败 | 属于宿主工具前提，不能计作源码修复失败。安装方预置与平台匹配的ripgrep并验证宿主原生搜索；不要重试下载直到成功，也不要把ARM64二进制装到其他平台。`preflight.py`的`host_search`只报告PATH发现，不证明搜索已经执行；harness解析本身不依赖rg，已加载技能后的源码查询可使用可用的grep/find或Python |

缺CANN时Linux仍可处理已有日志和静态信息，编译探针不可用。环境检查项应与首个源码错误分别读取；缺toolkit不能覆盖已有编译日志证据。版本未知保持unknown，版本/arch敏感建议需核当前声明与真实编译来源。

`environment_unavailable`、`case_not_reproducible` 或中断不能证明修复成败。先读reason、构建日志、实际命令与覆盖证据，再按已有授权修复环境或构建配方。已明确CANN安装位置但环境变量未透传时，先修实际路径；只有记录了可用性证据时才考虑 `ACD_VERIFY_SKIP_ENV_CHECK=1`，它不让缺失工具或失败编译变为通过。

如共享引擎或KB漂移，使用完整一致的新发布包；使用者不要现场修改 `scripts/engine/` 或生成API投影。文件自检输出标识本次内容，不代替维护者的发布清单比对。
