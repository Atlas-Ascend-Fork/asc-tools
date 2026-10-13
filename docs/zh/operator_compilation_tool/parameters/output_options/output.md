# --output

## 功能说明

可选，编译输出的安装包路径+名称，如xxx/xxx/xxx.run，支持相对路径和绝对路径。不输入路径的情况下，在当前路径下生成；不输入安装包名称的情况下，静态shape安装包默认命名为`static_kernel_${datetime}_${pid}.run`，调试版本安装包默认命名为`debug_kernel_${datetime}.run`。简写为-o。

## 关联参数

无。

## 参数取值

**参数值**：编译输出的安装包路径+名称。

**参数值格式**：支持相对路径和绝对路径，支持大小写字母（a-z，A-Z）、数字（0-9）、下划线（\_）、中划线（-）、句点（.）、中文字符。

**默认值**：默认情况下在执行此工具的当前路径下生成`static_kernel_${datetime}_${pid}.run`/`debug_kernel_${datetime}.run`。

## 推荐配置及收益

无。

## 示例

```bash
op_compiler --op_params_dir=<dump_dir>  --soc_version=Ascendxxxx --log=info --job=128 --compile_mode=tune --output=$HOME/opcompile/out/
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
