# 错误类别 — 领域知识索引

本目录按诊断的 `category` 提供**根因 / 典型报错 / 修法 / 说明**。`phase`和`category`是检索线索，可能来自消息或路径推断；它们不直接证明失败函数的执行域，也不决定该改哪里。未识别或证据不足时保留unknown，结合首错、声明和实际构建上下文判读。

> 各页的「典型报错」为真实编译日志中的常见形态；具体修法以当前工程的声明与构建证据为准。
> 「修法」要点与随包引擎的分类建议同源（`scripts/engine/`）；跨版本、架构门控与
> API 存在性的事实见 [知识专题](../knowledge/README.md)。不臆造。

## Host与Kernel的判定依据

先定位首错所在函数及日志关联的声明，再核函数修饰、有效条件宏、实际编译命令的参数和目标，确定此次失败属于Host还是Device上下文。混合编译单元须定位到具体函数或声明，不能给整个文件统一定域；证据不足时保留unknown，再定向补证。

文件后缀、目录名、编译器名称和分类字段只作线索：`.asc`、`op_kernel`或ccec/bisheng本身不证明错误发生在Kernel，普通C++语义错误也可能发生在Kernel。确认当前调用域后再改声明、调用或数据传递，并沿原配方验证；不凭分类搬移逻辑或添加函数修饰。

## 16 页速查

| category | 一句话 | 典型 phase | 页 |
|---|---|---|---|
| cmake_config | CMake 配置阶段（find_package/LANGUAGES/module） | cmake_configure | [cmake-config.md](cmake-config.md) |
| environment | 环境变量/SoC/CANN 路径 | env_check | [environment.md](environment.md) |
| header_missing | 头文件缺失（含 kernel_operator.h） | unknown_compile | [header-missing.md](header-missing.md) |
| dependency_missing＊ | include 目录/生成头/依赖目标缺失（复合指引，主判落 header_missing/cmake_config/link_error） | cmake_generate | [dependency-missing.md](dependency-missing.md) |
| host_compile_error | Host 侧 C++ 编译错误 | host_compile | [host-compile-error.md](host-compile-error.md) |
| kernel_compile_error | 已核设备调用域中的编译错误 | kernel_compile | [kernel-compile-error.md](kernel-compile-error.md) |
| link_error | 链接错误（undefined symbol / cannot find -l） | link | [link-error.md](link-error.md) |
| api_incompatible | AscendC API 不兼容（改名/漂移/幻觉） | unknown_compile | [api-incompatible.md](api-incompatible.md) |
| template_error | C++ 模板/重载/转换错误 | unknown_compile | [template-error.md](template-error.md) |
| tiling_consistency | Tiling 字段 Host↔Kernel 不一致 | kernel_compile | [tiling-consistency.md](tiling-consistency.md) |
| schema_consistency | 算子描述（.json/.asc/Init）三处不一致 | host_compile | [schema-consistency.md](schema-consistency.md) |
| compiler_limit | 触发编译器限制（跳转范围/展开/模板深度） | kernel_compile | [compiler-limit.md](compiler-limit.md) |
| compiler_internal | 疑似编译器内部错误（ICE/崩溃） | kernel_compile | [compiler-internal.md](compiler-internal.md) |
| unfriendly_error_code | 编译器拒绝非法选项/arch（措辞不友好） | kernel_compile | [unfriendly-error-code.md](unfriendly-error-code.md) |
| cann_error_code | CANN 专有错误码（编译期/运行期分流） | unknown_compile | [cann-error-code.md](cann-error-code.md) |
| package_error | 打包阶段（kernel metadata / json 未生成） | package | [package-error.md](package-error.md) |

> 注：诊断引擎的签名映射共产出 16 个 category 名。`cann_error_code` 有独立页（见上表）；
> `downstream_error` 并入 [链接错误](link-error.md)；
> `compiler_internal_or_cann_internal` 并入 [编译器内部错误](compiler-internal.md)；
> `dependency_missing` 不是引擎独立产出的分类名，其页为复合成因指引（＊标注）。
