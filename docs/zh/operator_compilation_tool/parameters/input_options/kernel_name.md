# --kernel\_name

## 功能说明

和[--op\_params\_dir](op_params_dir.md)二选一必选，设置需要编译的调试版本的算子kernel\_name。简写为-k。

kernel\_name的获取方式如下：算子执行过程中出现报错，根据报错日志可以获取到出错算子的kernel\_name（搜索kernel\_name关键字）。报错日志的获取方式请参考《[日志参考](https://gitcode.com/cann/runtime/blob/master/docs/zh/log_ref/README.md)》。

## 关联参数

和[--op\_debug\_config](../debug_options/op_debug_config.md)参数配合使用。仅在编译调试版本的二进制时，可以使用该参数。

## 参数取值

在算子报错日志中搜索kernel\_name关键字，找到报错的kernel\_name。

## 推荐配置及收益

无。

## 示例

```bash
op_compiler --kernel_name=xxx  --soc_version=<soc_version>
```

## 支持的型号

<!-- npu="310p" id1 -->
- Atlas推理系列产品
<!-- end id1 -->
<!-- npu="910" id2 -->
- Atlas训练系列产品
<!-- end id2 -->
<!-- npu="910b" id3 -->
- Atlas A2系列产品
<!-- end id3 -->
<!-- npu="950" id4 -->
- Ascend 950PR&950DT系列产品
<!-- end id4 -->

## 依赖约束

无。
