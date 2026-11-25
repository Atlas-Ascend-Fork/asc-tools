# 错误类别 — 本地取证与知识查询路由索引

本目录按诊断输出的 `category` 提供**诊断信号消歧、本地确定性取证动作、`cannbot-knowledge` 查询路由与契约保护红线**。`phase` 与 `category` 是日志检索线索，不单独证明失败函数的真实执行域或唯一修复位置；证据不足时保留 `unknown` 并定向补证。

> 包内不维护静态 API 签名表、跨代硬件容量表或版本迁移百科。涉及领域知识时，由宿主按各页路由加载上游查询技能，并与本机有效 SDK 声明及实际编译命令交叉核实，详见[知识协作契约](../cannbot-contract.md)。

## Host与Kernel的判定依据

先定位首错所在函数及日志关联的声明，再核对函数修饰符、有效条件宏、实际编译命令参数与编译目标，确认此次失败属于 Host 还是 Device 调用域。混合编译单元须逐函数或逐声明定域，不能对整个文件一概而论；证据不足时保留 `unknown` 后补取最小区分证据。

文件后缀、目录名、编译器可执行文件名和分类字段仅作线索：`.asc`、`op_kernel` 或 `bisheng`/`ccec` 本身不证明错误发生在设备核函数，常规 C++ 语法或类型错误同样可能出现在设备侧代码中。确认真实调用域后再修改声明、调用或数据传递，不凭分类名盲目搬移逻辑或增删限定符。

## 16 类取证与路由速查

| category | 诊断定位焦点 | 典型 phase | 页 |
|---|---|---|---|
| `cmake_config` | CMake 配置、语言模块与构建规则依赖 | `cmake_configure` | [cmake-config.md](cmake-config.md) |
| `environment` | 工具链路径、目标 SKU 与环境变量一致性 | `cmake_configure` | [environment.md](environment.md) |
| `header_missing` | 头文件查找、编译语言路由与包含用途判别 | `unknown_compile` | [header-missing.md](header-missing.md) |
| `dependency_missing`＊ | 生成头、导出目录与跨目标构建顺序（复合指引） | `cmake_generate` | [dependency-missing.md](dependency-missing.md) |
| `host_compile_error` | 已核 Host 调用域的声明、签名与接口错误 | `host_compile` | [host-compile-error.md](host-compile-error.md) |
| `kernel_compile_error` | 已核设备调用域的语言约束、地址空间与接口错误 | `kernel_compile` | [kernel-compile-error.md](kernel-compile-error.md) |
| `link_error` | 符号定义、导出可见性、名字修饰与库搜索闭合 | `link` | [link-error.md](link-error.md) |
| `api_incompatible` | 命名空间导出、版本接口演进与目标条件门控 | `unknown_compile` | [api-incompatible.md](api-incompatible.md) |
| `template_error` | 模板推导、重载决议、类型贯通与实例化约束 | `unknown_compile` | [template-error.md](template-error.md) |
| `tiling_consistency` | Host 生产与 Kernel 消费的共享数据结构契约 | `kernel_compile` | [tiling-consistency.md](tiling-consistency.md) |
| `schema_consistency` | 算子原型定义、代码生成物与消费方映射一致性 | `host_compile` | [schema-consistency.md](schema-consistency.md) |
| `compiler_limit` | 跳转跨度、模板深度、编译期资源断言与优化敏感 | `kernel_compile` | [compiler-limit.md](compiler-limit.md) |
| `compiler_internal` | 编译器后端崩溃/ICE 核实、前序错误排查与最小复现 | `kernel_compile` | [compiler-internal.md](compiler-internal.md) |
| `unfriendly_error_code` | 非法架构参数或编译器选项拒绝（优先于伴随崩溃） | `kernel_compile` | [unfriendly-error-code.md](unfriendly-error-code.md) |
| `cann_error_code` | 编译链错误码归属判别与上游知识取证 | `unknown` | [cann-error-code.md](cann-error-code.md) |
| `package_error` | 算子包元信息、导出符号与描述文件生成闭合 | `package` | [package-error.md](package-error.md) |

> 注：引擎规则见 `scripts/engine/harness/config/error_signatures.yaml`。`downstream_error` 并入 [link-error.md](link-error.md)；`compiler_internal_or_cann_internal` 并入 [compiler-internal.md](compiler-internal.md)；`dependency_missing`（＊）为跨阶段复合排查页。
