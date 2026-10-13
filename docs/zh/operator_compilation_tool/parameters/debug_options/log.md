# --log

## 功能说明

设置算子编译过程中日志的级别。简写为-l。

- **日志落盘**

    日志默认落盘到如下路径：`$HOME/ascend/log/debug/plog/plog-pid_*.log`

    pid代表进程ID，“\*”表示该日志文件创建时的时间戳。

- **日志打印**

    设置日志级别，开启日志功能后，日志默认不打印，如需打印显示，则请在执行op\_compiler命令的当前窗口设置如下环境变量，然后再执行op\_compiler命令：

    ```bash
    export ASCEND_SLOG_PRINT_TO_STDOUT=1
    ```

## 关联参数

无。

## 参数取值

**参数值**：

- debug：输出debug/info/warning/error/event级别的运行信息。
- info：输出info/warning/error/event级别的运行信息。
- warning：输出warning/error/event级别的运行信息。
- error：输出error/event级别的运行信息。

**参数默认值**：默认情况下，不输出日志。

## 推荐配置及收益

无。

## 示例

```bash
op_compiler --op_params_dir=<dump_dir>  --soc_version=<soc_version> --log=debug
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
