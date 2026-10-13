# --super\_kernel\_debug\_options

## 功能说明

开启Super Kernel功能后，设置SuperKernel静态Kernel编译时使用的调试选项。

本参数接收一个结构化字典字符串，支持标准JSON object或Python字典字面量。

## 关联参数

- 必须与--enable\_super\_kernel同时使用。
- 可以单独使用，也可以和--super\_kernel\_optimize\_options同时使用。
- 不能与--super\_kernel\_options同时使用。用户只能选择原有合并字符串接口，或者选择一个或两个新的结构化接口。
- 与--super\_kernel\_optimize\_options同时使用时，两个字典不能包含相同的顶层key。

## 参数取值

--super\_kernel\_debug\_options等号（或空格）后面跟一个字符串。字符串内容必须能够解析为字典，顶层对象不能是list、字符串、数字或null/None。字典key必须使用字符串，value应严格使用对应SuperKernel选项支持的类型和取值。

支持以下两种写法：

```bash
# 写法一：外层单引号，内层双引号，内容为标准JSON。推荐使用。
--super_kernel_debug_options='{"debug_sync_all":0}'

# 写法二：外层双引号，内层单引号，内容为Python字典字面量。
--super_kernel_debug_options="{'debug_sync_all':0}"
```

可输入option请参见《[TorchAir使用指南](https://www.hiascend.com/document/detail/zh/Pytorch/latest/devguide/TorchAir/docs/zh/overview.md)》。

## 推荐配置及收益

无。

## 示例

```bash
op_compiler --op_params_dir=<dump_dir>  --soc_version=<soc_version>  --enable_super_kernel --super_kernel_debug_options='{"debug_sync_all":1,"debug_op_exec_trace":1}'
```

## 支持的型号

<!-- npu="310p" id1 -->
- Atlas推理系列产品，不支持该参数
<!-- end id1 -->
<!-- npu="910" id2 -->
- Atlas训练系列产品，不支持该参数
<!-- end id2 -->
<!-- npu="910b" id3 -->
- Atlas A2系列产品，支持该参数
<!-- end id3 -->
<!-- npu="950" id4 -->
- Ascend 950PR&950DT系列产品，不支持该参数
<!-- end id4 -->

## 依赖约束

该选项的使能通常不由用户直接控制。

在TorchAir层会识别用户的代码或者手动开启Super Kernel对应的选项，只有在TorchAir明确传递了Super Kernel使能信息且添加了Super Kernel选项的时候，算子编译工具才会添加该输入选项。
