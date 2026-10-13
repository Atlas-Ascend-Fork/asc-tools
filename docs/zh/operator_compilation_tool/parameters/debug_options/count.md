# --count

## 功能说明

和[--op\_params\_dir](../input_options/op_params_dir.md)参数配合使用，用于统计[--op\_params\_dir](../input_options/op_params_dir.md)参数指定的目录下算子信息统计json文件的数目。

## 关联参数

和[--op\_params\_dir](../input_options/op_params_dir.md)参数配合使用。

## 参数取值

无

## 推荐配置及收益

无。

## 示例

```bash
op_compiler -p <dump_dir> --count
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

无
