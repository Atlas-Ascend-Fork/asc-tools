# 参数概览

**表 1**  算子编译工具参数概览

| OPC参数名称 | 参数简述（具体说明见参数描述章节） | 是否必选 | 默认值 |
| --- | --- | --- | --- |
| [--help](help_options/help.md) | 显示帮助信息。 | 否 | 不涉及 |
| [--op_params_dir](input_options/op_params_dir.md) | Dump工具导出的统计数据所在的文件夹路径，支持绝对和相对路径。 | 是（[--op_params_dir](input_options/op_params_dir.md)和[--kernel_name](input_options/kernel_name.md)二选一） | 不涉及 |
| [--kernel_name](input_options/kernel_name.md) | 算子kernel_name。仅支持调试版本编译，需要和--op_debug_config参数配合使用。 | 是（[--op_params_dir](input_options/op_params_dir.md)和[--kernel_name](input_options/kernel_name.md)二选一） | 不涉及 |
| [--job](input_options/job.md) | 编译时工作进程数。最小取值为1。 | 否 | 16 |
| [--compile_mode](input_options/compile_mode.md) | 取值支持配置为tune，代表开启调优模式，执行调优编译流程。不使用该参数的情况下，执行默认编译流程。**调优功能自CANN 9.1.0版本起废弃，配置tune后调优模式不生效，自动切换为默认编译流程。** | 否 | 不涉及 |
| [--enable_super_kernel](input_options/enable_super_kernel.md) | 是否开启Super Kernel功能。 | 否 | 默认不使能Super Kernel。 |
| [--super_kernel_options](input_options/super_kernel_options.md) | 开启Super Kernel功能后，可通过该选项添加Super Kernel编译选项。 | 否 | 不涉及 |
| [--super_kernel_optimize_options](input_options/super_kernel_optimize_options.md) | 开启Super Kernel功能后，设置SuperKernel静态Kernel编译时使用的融合优化选项。 | 否 | 不涉及 |
| [--super_kernel_debug_options](input_options/super_kernel_debug_options.md) | 开启Super Kernel功能后，设置SuperKernel静态Kernel编译时使用的调试选项。 | 否 | 不涉及 |
| [--follow_op_params_jit](input_options/follow_op_params_jit.md) | 该选项为可选配置。启用后，编译系统将根据所dump算子json文件中的"deterministic"字段，自动选择确定性或非确定性编译模式（二选一），从而有效提升编译性能。 | 否 | 不涉及 |
| [--output](output_options/output.md) | 编译输出的安装包路径+名称，如xxx/xxx/xxx.run，支持相对路径和绝对路径。 | 否 | 工具执行的当前路径下生成static_kernel_${datetime}_${pid}.run/debug_kernel_${datetime}.run |
| [--soc_version](target_chip_options/soc_version.md) | 指定算子编译时AI处理器的型号。 | 是（仅执行算子编译功能时为必选） | 不涉及 |
| [--log](debug_options/log.md) | 设置算子编译过程中日志的级别。 | 否 | null |
| [--count](debug_options/count.md) | 统计[--op_params_dir](input_options/op_params_dir.md)参数指定的目录下算子信息统计json文件的数目。 | 否 | 不涉及 |
| [--op_debug_config](debug_options/op_debug_config.md) | 调试配置文件的路径及文件名，路径支持绝对和相对路径。 | 否 | 不涉及 |
