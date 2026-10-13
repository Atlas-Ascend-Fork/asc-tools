# 算子编译工具用户指南

- [简介](introduction.md)
- [使用方法](usage/usage.md)
  - [使用前准备](usage/preparation.md)
  - [静态shape编译](usage/static_shape_compilation/static_shape_compilation.md)
    - [算子编译生成Kernel包](usage/static_shape_compilation/kernel_package_generation.md)
    - [Kernel包安装和卸载](usage/static_shape_compilation/kernel_package_install_uninstall.md)

  - [调试版本编译](usage/debug_version_compilation/debug_version_compilation.md)
    - [算子编译生成Kernel包](usage/debug_version_compilation/kernel_package_generation.md)
    - [Kernel包安装和卸载](usage/debug_version_compilation/kernel_package_install_uninstall.md)

- [参数说明](parameters/parameters.md)
  - [参数概览](parameters/parameter_overview.md)
  - [帮助选项](parameters/help_options/help_options.md)
    - [--help](parameters/help_options/help.md)

  - [输入选项](parameters/input_options/input_options.md)
    - [--op\_params\_dir](parameters/input_options/op_params_dir.md)
    - [--kernel\_name](parameters/input_options/kernel_name.md)
    - [--job](parameters/input_options/job.md)
    - [--compile\_mode](parameters/input_options/compile_mode.md)
    - [--enable\_super\_kernel](parameters/input_options/enable_super_kernel.md)
    - [--super\_kernel\_options](parameters/input_options/super_kernel_options.md)
    - [--super\_kernel\_optimize\_options](parameters/input_options/super_kernel_optimize_options.md)
    - [--super\_kernel\_debug\_options](parameters/input_options/super_kernel_debug_options.md)
    - [--follow\_op\_params\_jit](parameters/input_options/follow_op_params_jit.md)

  - [输出选项](parameters/output_options/output_options.md)
    - [--output](parameters/output_options/output.md)

  - [目标芯片选项](parameters/target_chip_options/target_chip_options.md)
    - [--soc\_version](parameters/target_chip_options/soc_version.md)

  - [调试选项](parameters/debug_options/debug_options.md)
    - [--log](parameters/debug_options/log.md)
    - [--count](parameters/debug_options/count.md)
    - [--op\_debug\_config](parameters/debug_options/op_debug_config.md)
