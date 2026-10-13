# 算子编译生成Kernel包

在任意目录下，以运行用户（如HwHiAiUser）身份执行如下命令，进行算子编译：

传入dump的算子信息统计文件进行调试版本编译，编译命令样例：

```bash
op_compiler --op_params_dir=<dump_dir>  --op_debug_config=gm_debug.cfg --soc_version=<soc_version> --log=info --job=128 --output=<output_dir>
```

或

```bash
op_compiler -p <dump_dir>  -d gm_debug.cfg -v <soc_version> -l info -j 128 -o <output_dir>
```

传入kernel\_name进行调试版本编译，编译命令样例：

```bash
op_compiler --kernel_name=<kernel_name>  --op_debug_config=gm_debug.cfg --soc_version=<soc_version> --log=info --job=128  --output=<output_dir>
```

或

```bash
op_compiler -k <kernel_name>  -d gm_debug.cfg -v <soc_version> -l info -j 128 -o <output_dir>
```

- --op\_params\_dir和--kernel\_name，两个参数只能使用一个，为必选参数。在报错日志中能够获取到明确的算子kernel\_name时，建议使用kernel\_name参数。
    - --op\_params\_dir：简写为-p，Dump工具导出的统计数据所在的文件夹路径，支持绝对和相对路径。目前支持两种方式Dump算子json文件，请根据实际情况选择合适的方式。
        - **使用PyTorch的Python接口编程时**，可通过Ascend PyTorch Profiler接口dump算子json文件，具体请参考《[Ascend Pytorch调优工具](https://www.hiascend.com/document/detail/zh/mindstudio/latest/msTT_msIT/ascend_pytorch_profiler/docs/zh/developer_notes/ascend_pytorch_profiler_user_guide.md)》。
            1. 使用Ascend PyTorch Profiler接口开启PyTorch训练时的性能数据采集。

                在训练前，开启扩展参数experimental\_config中“算子信息统计功能”，即参数record\_op\_args置为True。

            2. 查看采集到的PyTorch训练性能数据结果文件。

                训练结束后，Dump的算子信息统计文件默认在`{worker_name}_{时间戳}_ascend_pt_op_args/{pid}_debug`目录下。

        - **用acl接口编程时**，可通过aclopStartDumpArgs和aclopStopDumpArgs接口将算子信息统计文件Dump到指定目录下。接口的详细介绍请参考《[Runtime运行时API](https://gitcode.com/cann/runtime/blob/9.2.0/docs/zh/api_ref/README.md)》。

    - --kernel\_name：简写为-k，在算子执行的报错日志中获取kernel\_name。

- --op\_debug\_config：简写为-d，调试配置文件的路径及文件名，路径支持绝对和相对路径。参数使用示例如下：

    ```bash
    --op_debug_config=$HOME/module/gm_debug.cfg
    ```

    配置文件的内容示例如下，具体的配置方法请参考[--op\_debug\_config](../../parameters/debug_options/op_debug_config.md)。

    ```text
    op_debug_config=ccec_g,oom
    ```

- --soc\_version：简写为-v，执行算子编译功能时必选，配置为算子编译时AI处理器的型号。

    > [!NOTE]说明
    >如果无法确定当前设备的soc\_version，则在安装NPU驱动包的服务器执行**npu-smi info**命令进行查询，在查询到的“Name”前增加Ascend信息，例如“Name”对应取值为xxxyy，实际配置的soc\_version值为Ascendxxxyy。

- --log：简写为-l，可选，设置算子编译过程中日志的级别。可设置为debug/info/warning/error/null级别，默认为null。
- --job：简写为-j，可选，设置编译时工作进程数。最小取值为1，默认为16。
- --output：简写为-o，可选，编译输出的安装包路径+名称，如xxx/xxx/xxx.run，支持相对路径和绝对路径。不输入路径的情况下，在当前路径下生成；不输入安装包名称的情况下，安装包默认命名为`debug_kernel_${datetime}.run`。

工具支持的全量参数的具体说明可参见[参数说明](../../parameters/parameters.md)。

当出现类似如下回显信息代表编译成功。

```text
generate run package debug_kernel_${datetime}.run success
```

> [!NOTE]说明
>不支持在`dump_dir`下执行编译命令。
