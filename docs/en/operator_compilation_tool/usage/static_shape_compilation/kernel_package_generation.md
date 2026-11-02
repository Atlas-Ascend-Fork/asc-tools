# Compiling an Operator to Generate a Kernel Package

<!-- md-trans-meta sourceCommit=681b3fab3b2117829dc3976c9606bfc0f3d15012 translatedAt=2026-10-14T08:56:47.705Z pushedAt=2026-10-15T11:15:08.880Z -->

1. Prepare the dumped operator information statistics file. Currently, two methods are supported for dumping operator JSON files. Select an appropriate method based on the actual situation.
    - **When programming with PyTorch Python APIs**, you can dump operator JSON files through the Ascend PyTorch Profiler API. For details, see *Ascend PyTorch Profiler*.
        1. Use the Ascend PyTorch Profiler API to enable performance data collection during PyTorch training.

            Before training, enable the operator information statistics function in the extended parameter **experimental\_config**, that is, set the **record\_op\_args** parameter to **True**.

        2. View the collected PyTorch training performance data result file.

            After training, the dumped operator information statistics files are stored in the **\{worker\_name\}\_*\{timestamp\}*\_ascend\_pt\_op\_args/*\{pid\}*** directory by default.

    - **When programming with ACL APIs**, you can dump the operator information statistics files to a specified directory through the **aclopStartDumpArgs** and **aclopStopDumpArgs** APIs.

2. In any directory, run the following command as the running user (for example, **HwHiAiUser**) to compile the operator:

    Example compilation command:

    ```bash
    op_compiler --op_params_dir=<dump_dir>  --soc_version=<soc_version> --log=info --job=8 --output=<output_dir>
    ```

    or

    ```bash
    op_compiler -p <dump_dir>  -v <soc_version> -l info -j 8 -o <output_dir>
    ```

    - **--op\_params\_dir**: abbreviated as **-p**, required. This parameter specifies the path of the folder where the statistics data exported by the Dump tool is stored. Both absolute and relative paths are supported.
    - **--soc\_version**: abbreviated as **-v**, required when compiling operators. This parameter specifies the AI processor model used for operator compilation.

        > [!NOTE] Note
        > If you cannot determine the **soc\_version** of the current device, run the **npu-smi info** command on the server where the NPU driver package is installed to query it. Add **Ascend** before the queried **Name** value. For example, if the **Name** value is **_xxxyy_**, the actual **soc\_version** value to configure is **Ascend_xxxyy_**.

    - **--log**: abbreviated as **-l**, optional. This parameter sets the log level during operator compilation. The value can be **debug**, **info**, **warning**, **error**, or **null**, and the default value is **null**.
    - **--job**: abbreviated as **-j**, optional. This parameter sets the number of worker processes during compilation. The minimum value is **1**, and the default value is **16**.
    - **--output**: abbreviated as **-o**, optional. This parameter specifies the path and name of the installation package output during compilation, for example, ***xxx*/*xxx*/*xxx*.run**. Both relative and absolute paths are supported. If no path is specified, the package is generated in the current path. If no package name is specified, the package is named **static\_kernel\_*$\{datetime\}*\_*$\{pid\}*.run** by default.

    For details about all parameters supported by the tool, see [Parameters](../../parameters/parameters.md).

    Compilation is successful when a message similar to the following is displayed.

    ```text
    generate run package static_kernel_${datetime}_${pid}.run success
    ```

> [!NOTE] Note
>
> - The operator compilation tool provides the **--count** parameter, which is used together with the **-p** parameter to count the number of operator information statistics JSON files in the directory specified by **-p**.
> The following is an example:
>
> ```bash
> op_compiler -p <dump_dir> --count
>    ```
>
> Only dynamic shapes can dump operator statistics. After a static kernel package is installed, the statistics of the operators corresponding to the static kernel package are no longer dumped. Therefore, after installing a static kernel package, if the network changes, you can determine whether the static kernel package matches the current network by comparing the number of dumped JSON files before and after the change.
> Perform a dump operation before and after the network change, and use the **--count** command to count the number of JSON files generated during the dump. If the number after the change is greater than that before the change, some operators in the static kernel package no longer match the current network. In this case, you can:
>
> - Uninstall the static kernel package, perform the dump process again, and compile and install a new static kernel package.
> - Continue to use the current static kernel package. Note that the unmatched operators will go through the dynamic process and will not benefit from performance gains.
> - Compilation commands cannot be executed in **_dump\_dir_**.
