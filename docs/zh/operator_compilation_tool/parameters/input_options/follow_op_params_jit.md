# --follow\_op\_params\_jit

## 功能说明

该选项为可选配置。启用后，编译系统将根据所dump算子json文件中的 "deterministic" 字段，自动选择确定性或非确定性编译模式（二选一），从而有效提升编译性能。简写为-f。

"deterministic"字段为"true"表示确定性开关打开，为“false”表示确定性开关关闭。默认情况，即该选项不配置时，不会读取json文件中的"deterministic"字段，会编译两份算子kernel，一份为确定性开关打开的算子kernel，一份确定性开关关闭的算子kernel。

## 关联参数

无。

## 参数取值

无。

## 推荐配置及收益

无。

## 示例

```bash
op_compiler --op_params_dir=<dump_dir>  --soc_version=<soc_version>  --follow_op_params_jit
```

## 支持的型号

<!-- npu="310p" id1 -->
- Atlas推理系列产品，支持该参数
<!-- end id1 -->
<!-- npu="910" id2 -->
- Atlas训练系列产品，支持该参数
<!-- end id2 -->
<!-- npu="910b" id3 -->
- Atlas A2系列产品，支持该参数
<!-- end id3 -->
<!-- npu="950" id4 -->
- Ascend 950PR&950DT系列产品，支持该参数
<!-- end id4 -->

## 依赖约束

无。
