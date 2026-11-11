# FP8/MX 类型的架构门控

排查 FP8 / MX matmul 的编译失败时，先核对存储类型的目标映射，以及 MX 搬运/计算入口的声明和实现门控。这些证据能帮助区分目标不支持与类型、参数、头入口等问题；仍需结合完整首错、实际编译参数和当前 SDK，不能仅凭出现 FP8/MX 名称就断定是架构问题。

## 类型矩阵（声明面）

| 层 | 2201（910B/B1-B4） | 3002 | 3510（950 系） |
|---|---|---|---|
| `TypeFromId` 存储类型（utils/tiling/template_argument.h） | 仅 BF16 新增 | BF16/HiF8/FP8(E5M2,E4M3FN)/FP4(E2M1,E1M2) | 同 3002 另加 complex64/32 |
| FP8/FP4 真 builtin 别名（impl/utils/common_types.h） | 无（退化 uint8_t） | 无（退化 uint8_t） | 有（9.0 还含 5102，9.1 收窄仅 3510） |
| MX matmul 机器面（MmadMx/带 scale 的 LoadData 重载/mx_matmul_impl） | 无 | 无 | 有（9.0 的 MX LoadData 含 5102，9.1 收窄；MmadMx 是 9.1 新增） |

在本页核实的 9.0/9.1 头文件中，2201 的 `TypeFromId` 分支没有这些 FP8/FP4 映射，也没有对应 MX 计算实现。某些类型名仍可能作为 `uint8_t` 别名可见，这不代表 FP8 数值语义或 MX 运算受支持。任务明确要求 910B 时，先核对能保留原数值语义的实现；更换 dtype 或目标会改变任务约束，不能作为默认修法。

## mx_ 前缀类型与存储类型的区别

`mx_fp8_e4m3_t` / `mx_fp8_e5m2_t` / `mx_fp8_e8m0_t` 定义在 `impl/basic_api/utils/kernel_utils_constants.h`（namespace AscendC，形如 `using mx_fp8_e4m3_t = struct {};`，9.0.0 与 9.1.0 均在）。它们是**不承载数值数据的空结构体标签，不是存储 dtype**；空结构体不意味着 `sizeof` 为零。它们用作 MX matmul 中 L0A/L0B 矩阵的模板实参，由 `GetDstType` 映射回存储类型（`mx_fp8_e4m3_t→fp8_e4m3fn_t`、 `mx_fp8_e5m2_t→fp8_e5m2_t`、`mx_fp8_e8m0_t→fp8_e8m0_t`）， `GetL0DataType<fp8_e4m3fn_t, true>` 反向给出 mx 标签（3510 分支）。

- 存储面（GlobalTensor/UB）用 `fp8_e4m3fn_t` / `fp8_e5m2_t`；L0 矩阵面用对应
`mx_` 前缀标签。Load2D MX 规则（9.0.0 docs Load2DMX.md）：src 为 fp8_e4m3fn_t/fp8_e5m2_t 时 dst 须用对应 `AscendC::mx_fp8_*_t`，否则编译失败。
- 直接拿存储类型当 L0 矩阵（或反之）触发 `SupportType` 断言：
`LoadData 2dv2 with scale matrix only support fp4/fp8 dtype on current device!` （dav_3510/kernel_operator_mm_impl.h:312/362）或 `MmadMx not support data type on current device!`（同文件 :608/627/653/675）。
- Mmad 的 MX dtype 白名单（dav_3510 实现）：`Tuple<float, mx_fp8_e4m3_t|mx_fp8_e5m2_t, mx_fp8_e4m3_t|mx_fp8_e5m2_t>` 四种组合 + FP4 对。
- `DT_FLOAT8_E8M0(=38)` 是枚举预留位，无 `TypeFromId` 映射；scale 用类型名
`fp8_e8m0_t`（存储别名），与 `DT_` 码不是一回事。

## 910B1（2201）上的典型报错形态

- `MmadMx` 在 `basic_api/kernel_operator_mm_intf.h` 无条件声明，但定义按
`#if __NPU_ARCH__ == 3510` 门控（impl/basic_api/kernel_operator_mm_intf_impl.h:386）： 2201 上实例化即 `implicit instantiation of undefined template 'AscendC::MmadMx<…>'` 或链接期 undefined reference——声明可见不等于本目标有实现。
- 带 `srcMx` 的 LoadData 重载整个按 3510（9.0 为 3510/5102）门控不声明：
2201 上是 `no matching function for call to 'LoadData'`。
- `fp8_e4m3fn_t` 在非 3510 面（common_types.h）**静默退化为 uint8_t 别名**：
不报类型错不等于数值语义对；把它当 FP8 用是真错。
- 走 `TypeFromId<DT_FLOAT8_E4M3FN>` 路线：2201 未启用该 FP8 特化。已核 v9.1 头中的通用
`TypeFromId` 提供 `using type = void`；后续使用可能触发类型或实例化错误，不能固定归纳为
`no member named 'type'`。核对当前安装的映射与实际下游用法后再解释首错。

诊断口径：真实编译 argv 指向 2201 时，沿当前 SDK 的声明和实现确认该具体调用是否被门控排除，再判断是否属于目标能力限制。保留用户已确定的目标和数值要求；不能用正则命中替代适用性核实。随包引擎的规则 `mx_fp8_arch_scope_mismatch`（`scripts/engine/harness/knowledge/error_signatures.yaml`）只提供条件建议。

## 如何核实（本机安装头）

把 `<HEAD>` 换成当前安装的 Ascend C 头根（如 `$ASCEND_HOME_PATH` 下的 ascendc include 树；impl/ 是内部头，只用于核实，不建议直接 include）：

```bash
grep -rn "using mx_fp8" <HEAD> | head          # mx_ 标签类型定义与位置
grep -n "ASC_INNER_DATATYPE_ENABLE_FLOAT8" <HEAD>/utils/tiling/template_argument.h   # 逐 arch 分支
grep -n -B3 -A8 "using fp8_e4m3fn_t" <HEAD>/impl/utils/common_types.h               # 真别名 vs uint8_t 退化
grep -n "MmadMx\|srcMx" <HEAD>/include/basic_api/kernel_operator_mm_intf.h          # 声明与门控
```

同时固定报错编译单元和真实编译命令中的架构选项（例如 `--npu-arch=dav-2201` 或 `--npu-arch=dav-3510`）；宿主 NPU 型号不等于编译目标。目标已明确时不建议改目标，见 [2201/3510 迁移核验](arch-migration-2201-3510.md)；头入口条件的一般判读见 [头入口与目标](soc-arch-gating.md)；类型声明条目调用 CANNBOT `ascendc-docs-search` 技能来检索。

本页矩阵为声明面证据（按 v9.0.0 与 v9.1.0 安装树核实）；具体安装以实际头文件为准。
