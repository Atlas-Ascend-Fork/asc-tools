# Parameter Overview

<!-- md-trans-meta sourceCommit=681b3fab3b2117829dc3976c9606bfc0f3d15012 translatedAt=2026-10-14T08:42:20.932Z pushedAt=2026-10-15T11:15:08.860Z -->

**Table 1**  Operator compilation tool parameter overview

| OPC Parameter | Description (See the Parameter Description Section for Details) | Required | Default Value |
| --- | --- | --- | --- |
| [--help](help_options/help.md) | Displays help information. | No | N/A |
| [--op_params_dir](input_options/op_params_dir.md) | Path of the folder where the statistics data exported by the Dump tool is stored. Both absolute and relative paths are supported. | Yes (choose one of the two: [--op_params_dir](input_options/op_params_dir.md) or [--kernel_name](input_options/kernel_name.md)) | N/A |
| [--kernel_name](input_options/kernel_name.md) | **kernel_name** of the operator. Only debug version compilation is supported, and this parameter must be used together with **--op_debug_config**. | Yes (choose one of the two: [--op_params_dir](input_options/op_params_dir.md) or [--kernel_name](input_options/kernel_name.md)) | N/A |
| [--job](input_options/job.md) | Number of worker processes during compilation. The minimum value is **1**. | No | **16** |
| [--compile_mode](input_options/compile_mode.md) | The value can be set to **tune**, which enables the tuning mode and runs the tuning compilation process. If this parameter is not used, the default compilation process is executed. **The tuning function has been deprecated since CANN 9.1.0. After this parameter is set to tune, the tuning mode does not take effect and the default compilation process is automatically used.** | No | N/A |
| [--enable_super_kernel](input_options/enable_super_kernel.md) | Whether to enable the Super Kernel function. | No | Super Kernel is disabled by default. |
| [--super_kernel_options](input_options/super_kernel_options.md) | After the Super Kernel function is enabled, you can use this option to add Super Kernel compilation options. | No | N/A |
| [--follow_op_params_jit](input_options/follow_op_params_jit.md) | This option is optional. After it is enabled, the compilation system automatically selects the deterministic or non-deterministic compilation mode (choose one of the two) based on the **deterministic** field in the dump operator JSON file, thereby effectively improving compilation performance. | No | N/A |
| [--output](output_options/output.md) | Path and name of the installation package output by compilation, for example, ***xxx*/*xxx*/*xxx*.run**. Both relative and absolute paths are supported. | No | **static_kernel_*${datetime}*_*${pid}*.run** and **debug_kernel_*${datetime}*.run** are generated in the current path where the tool is executed |
| [--soc_version](target_chip_options/soc_version.md) | Specifies the model of the AI processor used for operator compilation. | Yes (required only when the operator compilation function is executed) | N/A |
| [--log](debug_options/log.md) | Sets the log level during operator compilation. | No | null |
| [--count](debug_options/count.md) | Counts the number of operator information statistics JSON files in the directory specified by [--op_params_dir](input_options/op_params_dir.md). | No | N/A |
| [--op_debug_config](debug_options/op_debug_config.md) | Path and file name of the debug configuration file. Both absolute and relative paths are supported. | No | N/A |
