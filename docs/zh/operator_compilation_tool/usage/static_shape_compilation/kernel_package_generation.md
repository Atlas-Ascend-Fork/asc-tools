# 算子编译生成Kernel包

1. 准备Dump的算子信息统计文件。目前支持两种方式Dump算子json文件，请根据实际情况选择合适的方式。
    - **使用PyTorch的Python接口编程时**，可通过Ascend PyTorch Profiler接口dump算子json文件，具体请参考《[Ascend Pytorch调优工具](https://www.hiascend.com/document/detail/zh/mindstudio/latest/msTT_msIT/ascend_pytorch_profiler/docs/zh/developer_notes/ascend_pytorch_profiler_user_guide.md)》。
        1. 使用Ascend PyTorch Profiler接口开启PyTorch训练时的性能数据采集。

            在训练前，开启扩展参数experimental\_config中“算子信息统计功能”，即参数**record\_op\_args**置为True。

        2. 查看采集到的PyTorch训练性能数据结果文件。

            训练结束后，Dump的算子信息统计文件默认在`{worker_name}_{时间戳}_ascend_pt_op_args/{pid}`目录下。

    - **使用acl接口编程时**，可通过aclopStartDumpArgs和aclopStopDumpArgs接口将算子信息统计文件Dump到指定目录下。接口的详细介绍请参考《[Runtime运行时API](https://gitcode.com/cann/runtime/blob/master/docs/zh/api_ref/README.md)》。

2. 在任意目录下，以运行用户（如HwHiAiUser）身份执行如下命令，进行算子编译：

    编译命令样例：

    ```bash
    op_compiler --op_params_dir=<dump_dir>  --soc_version=<soc_version> --log=info --job=8 --output=<output_dir>
    ```

    或

    ```bash
    op_compiler -p <dump_dir>  -v <soc_version> -l info -j 8 -o <output_dir>
    ```

    - --op\_params\_dir：简写为-p，必选，Dump工具导出的统计数据所在的文件夹路径，支持绝对和相对路径。
    - --soc\_version：简写为-v，执行算子编译功能时必选，指定算子编译时AI处理器的型号。

        > [!NOTE]说明
        >如果无法确定当前设备的soc\_version，则在安装NPU驱动包的服务器执行**npu-smi info**命令进行查询，在查询到的“Name”前增加Ascend信息，例如“Name”对应取值为xxxyy，实际配置的soc\_version值为Ascendxxxyy。

    - --log：简写为-l，可选，设置算子编译过程中日志的级别。可设置为debug/info/warning/error/null级别，默认为null。
    - --job：简写为-j，可选，设置编译时工作进程数。最小取值为1，默认为16。
    - --output：简写为-o，可选，编译输出的安装包路径+名称，如xxx/xxx/xxx.run，支持相对路径和绝对路径。不输入路径的情况下，在当前路径下生成；不输入安装包名称的情况下，安装包默认命名为`static_kernel_${datetime}_${pid}.run`。

    工具支持的全量参数的具体说明可参见[参数说明](../../parameters/parameters.md)。

    当出现类似如下回显信息代表编译成功。

    ```text
    generate run package static_kernel_${datetime}_${pid}.run success
    ```

> [!NOTE]说明
>
> - 算子编译工具提供了`--count`参数和-p参数配合使用，用于统计-p参数指定的目录下算子信息统计json文件的数目。
> 样例如下：
>
>    ```bash
>    op_compiler -p <dump_dir> --count
>    ```
>
>   只有动态shape才能dump出算子统计信息，安装静态Kernel包后，静态Kernel包对应算子的统计信息就不会dump出来。所以在安装静态Kernel包后，如果网络有调整，可以通过调整前后dump的json文件的数量来判断静态Kernel包和当前网络是否匹配。
>   通过调整网络前后，各执行一次dump操作，并通过--count命令来统计dump生成的json文件的数目，如果调整后的数目比调整前大，则说明静态Kernel包中有部分算子不再匹配当前网络，此时开发者可以：
>
>   - 卸载静态Kernel包，重新走dump流程，编译安装新的静态Kernel包。
>   - 仍使用当前静态Kernel包，此时需要注意不匹配的算子会走动态流程，得不到性能收益。
> - 不支持在`dump_dir`下执行编译命令。
