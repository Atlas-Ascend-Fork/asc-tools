# --soc\_version

## 功能说明

执行算子编译功能时必选，指定算子编译时AI处理器的型号。简写为-v。

## 关联参数

无。

## 参数取值

算子编译时AI处理器的型号。

如果无法确定当前AI处理器的型号，则在安装NPU驱动包的服务器执行**npu-smi info**命令进行查询，在查询到的“Name”前增加Ascend信息，例如“Name”对应取值为xxxyy，实际配置的soc\_version值为Ascendxxxyy。

## 推荐配置及收益

无。

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
