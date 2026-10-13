# --job

## 功能说明

设置编译时工作进程数。最小取值为1，默认为16。简写为-j。

## 关联参数

无

## 参数取值

设置编译时工作进程数。

## 推荐配置及收益

无。

## 示例

```bash
op_compiler --op_params_dir=<dump_dir>  --soc_version=<soc_version> --job=128
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
