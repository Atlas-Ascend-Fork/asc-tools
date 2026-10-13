# --enable\_super\_kernel

## 功能说明

是否开启Super Kernel功能。默认不使能Super Kernel，添加该选项时会修改编译行为，走Super Kernel流程。

## 关联参数

无。

## 参数取值

无

## 推荐配置及收益

无。

## 示例

```bash
op_compiler --op_params_dir=<dump_dir>  --soc_version=<soc_version>  --enable_super_kernel
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

在torchair层会识别用户的代码或者手动开启super kernel对应的选项，只有在torchair明确传递了Super Kernel使能信息的时候，op\_compiler才会添加该输入选项。
