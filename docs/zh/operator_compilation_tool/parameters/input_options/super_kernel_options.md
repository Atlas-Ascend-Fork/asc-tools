# --super\_kernel\_options

## 功能说明

开启Super Kernel功能后，可通过该选项添加Super Kernel编译选项。

## 关联参数

配置--enable\_super\_kernel选项后生效。

## 参数取值

--super\_kernel\_options等号（或空格）后面跟一个字符串，字符串由一个或多个key=value形式的选项拼接而成，多个选项之间以英文冒号 \`:\` 分隔。格式如下：

```bash
--super_kernel_options="key1=value1:key2=value2:key3=value3"
```

可输入option请参见《[TorchAir使用指南](https://www.hiascend.com/document/detail/zh/Pytorch/2620/devguide/TorchAir/docs/zh/overview.md)》。

## 推荐配置及收益

无。

## 示例

```bash
op_compiler --op_params_dir=<dump_dir>  --soc_version=<soc_version>  --enable_super_kernel --super_kernel_option="auto_op_parallel=1"
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
