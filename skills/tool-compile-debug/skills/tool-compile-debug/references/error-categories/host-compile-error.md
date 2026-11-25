# host_compile_error — Host 调用域声明、签名与边界取证

## 诊断信号与级联消歧

适用于已按[调用域判定](README.md#host与kernel的判定依据)确认为算子 **Host 调用域**（如算子注册、InferShape/InferDataType、Host Tiling 计算或启动封装）的 C++ 编译错误，包括声明与定义签名冲突、Host 框架上下文成员不匹配，或误在 Host 编译单元中直接引用设备侧专用枚举/内建符号。注意 `phase=host_compile` 仅为路径/规则推断线索：若首错实际位于设备核函数，转 [kernel-compile-error.md](kernel-compile-error.md)；若属 Host↔Kernel 共享 Tiling 字段错配，转 [tiling-consistency.md](tiling-consistency.md)；若属算子原型与生成接口失配，转 [schema-consistency.md](schema-consistency.md)。

## 本地确定性取证动作

1. **核对声明、定义与包含链**：用 `run.py inspect --project <工程根> --file <失败文件及对应头文件>` 对照 `.h` 与 `.cpp` 的函数名、命名空间、形参类型/个数及 `const` 限定。
2. **核对本机 SDK 的 Host 注册与上下文头**：用 `run.py inspect --question environment` 与 `--question build --build-dir <构建目录>` 确认实际 `-I` 搜索路径及当前 SDK 头文件中的 Host 类型成员签名。
3. **甄别跨域符号误用**：若未声明符号为同步原语或硬件流水枚举，先核实当前函数职责与执行域——设备同步留在设备侧保持配对关系，Host 侧流/事件管理使用对应的运行时接口，不凭未声明报错直接删掉同步逻辑。

## cannbot-knowledge 查询路由

| 未决领域问题 | 宿主加载技能 | 返回后本地核对项 |
|---|---|---|
| Host 侧算子注册、TilingContext / InferShape 或运行时接口签名不明 | cannbot-knowledge `ascendc-api-knowledge-query` 技能 | 读取正文并核对本机活跃 SDK 对应头文件中的类成员与函数原型 |
| Host/Device 职责划分、注册工程结构或跨版本 Host 接口变更 | cannbot-knowledge `knowledge-query` 技能 | 核对卡片适用版本与本工程当前构建配方、目标配置是否一致 |

## 契约保护红线

保持算子注册原型、形状推导逻辑、同步生命周期与错误处理契约；严禁用空实现桩函数、删除注册项或强转上下文指针换取编译通过。
