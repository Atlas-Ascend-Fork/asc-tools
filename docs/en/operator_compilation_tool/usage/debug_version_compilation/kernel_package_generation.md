# Compiling an Operator to Generate a Kernel Package

<!-- md-trans-meta sourceCommit=2e0f6e7cad32f84a05321a16885c71c448cd9b6f translatedAt=2026-10-30T10:07:22.955Z pushedAt=2026-11-03T00:55:22.820Z nl=2 -->

In any directory, run the following command as the running user (for example, **HwHiAiUser**) to compile an operator:

Pass the dump operator information statistics file to compile the debug version. Example compilation command:

```bash
op_compiler --op_params_dir=<dump_dir>  --op_debug_config=gm_debug.cfg --soc_version=<soc_version> --log=info --job=128 --output=<output_dir>
```

or

```bash
op_compiler -p <dump_dir>  -d gm_debug.cfg -v <soc_version> -l info -j 128 -o <output_dir>
```

Pass **kernel\_name** to compile the debug version. Example compilation command:

```bash
op_compiler --kernel_name=<kernel_name>  --op_debug_config=gm_debug.cfg --soc_version=<soc_version> --log=info --job=128  --output=<output_dir>
```

or

```bash
op_compiler -k <kernel_name>  -d gm_debug.cfg -v <soc_version> -l info -j 128 -o <output_dir>
```

- Either **--op\_params\_dir** or **--kernel\_name** is required. When the explicit operator **kernel\_name** can be obtained from the error log, you are advised to use the **kernel\_name** parameter.
    - **--op\_params\_dir**: abbreviated as **-p**. It specifies the path of the folder where the statistics data exported by the Dump tool is stored. Both absolute and relative paths are supported. Currently, two methods are supported for dumping operator JSON files. Select an appropriate method based on the actual situation.
        - **When programming with the Python APIs of PyTorch**, you can dump operator JSON files through the Ascend PyTorch Profiler API. For details, see [Ascend PyTorch Profiling Tool](https://gitcode.com/ascend/pytorch/blob/master/docs/zh/developer_notes/ascend_pytorch_profiler_user_guide.md).
            1. Use the Ascend PyTorch Profiler API to enable performance data collection during PyTorch training.

                Before training, enable the operator information statistics function in the extended parameter **experimental\_config**, that is, set the **record\_op\_args** parameter to **True**.

            2. View the collected PyTorch training performance data result file.

                After training, the dumped operator information statistics files are stored in the `{worker_name}_{timestamp}_ascend_pt_op_args/{pid}_debug` directory by default.

        - **When programming with ACL APIs**, you can dump the operator information statistics files to a specified directory through the **aclopStartDumpArgs** and **aclopStopDumpArgs** APIs. For details about the APIs, see [Runtime API](https://gitcode.com/cann/runtime/blob/master/docs/zh/api_ref/README.md).

    - **--kernel\_name**: abbreviated as **-k**. **kernel\_name** is obtained from the operator execution error log.

- **--op\_debug\_config**: abbreviated as **-d**, indicating the path and file name of the debug configuration file. Both absolute and relative paths are supported. An example of using this parameter is as follows:

    ```bash
    --op_debug_config=$HOME/module/gm_debug.cfg
    ```

    An example of the configuration file content is as follows. For details about the configuration method, see [--op\_debug\_config](../../parameters/debug_options/op_debug_config.md).

    ```text
    op_debug_config=ccec_g,oom
    ```

- **--soc\_version**: abbreviated as **-v**. This parameter is required when compiling operators. Set it to the model of the AI processor used for operator compilation.

    > [!NOTE] Note
    > If you cannot determine the **soc\_version** of the current device, run the **npu-smi info** command on the server where the NPU driver package is installed to query it. Add **Ascend** before the queried **Name** value. For example, if the **Name** value is **xxxyy**, the actual **soc\_version** value to configure is **Ascendxxxyy**.

- **--log**: abbreviated as **-l**, optional. This parameter sets the log level during operator compilation. The value can be **debug**, **info**, **warning**, **error**, or **null**. The default value is **null**.
- **--job**: abbreviated as **-j**, optional. This parameter sets the number of worker processes during compilation. The minimum value is **1**, and the default value is **16**.
- **--output**: abbreviated as **-o**, optional. This parameter specifies the path and name of the installation package output during compilation, for example, ***xxx*/*xxx*/*xxx*.run**. Both relative and absolute paths are supported. If no path is specified, the package is generated in the current path. If no package name is specified, the package is named `debug_kernel_${datetime}.run` by default.

For details about all parameters supported by the tool, see [Parameters](../../parameters/parameters.md).

Compilation is successful when a message similar to the following is displayed.

```text
generate run package debug_kernel_${datetime}.run success
```

> [!NOTE] Note
> Running the compilation command in `dump_dir` is not supported.
