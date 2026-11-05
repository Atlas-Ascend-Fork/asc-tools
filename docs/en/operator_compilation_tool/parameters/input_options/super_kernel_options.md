# --super\_kernel\_options

<!-- md-trans-meta sourceCommit=2e0f6e7cad32f84a05321a16885c71c448cd9b6f translatedAt=2026-10-30T10:06:04.301Z pushedAt=2026-11-03T00:55:22.817Z nl=2 -->

## Description

Adds Super Kernel compilation options after the Super Kernel feature is enabled.

## Related Parameters

This parameter takes effect after the **--enable\_super\_kernel** option is configured.

## Parameter Values

The equal sign (or space) after **--super\_kernel\_options** is followed by a string that consists of one or more options in the **key=value** format, with multiple options separated by colons (:). The format is as follows:

```bash
--super_kernel_options="key1=value1:key2=value2:key3=value3"
```

For available options, see [TorchAir User Guide*](https://gitcode.com/ascend/torchair/blob/master/docs/zh/overview.md).

## Recommended Configuration and Benefits

None

## Examples

```bash
op_compiler --op_params_dir=<dump_dir>  --soc_version=<soc_version>  --enable_super_kernel --super_kernel_options="auto_op_parallel=1"
```

## Supported Models

<!-- npu="310p" id1 -->
- Atlas inference products: This parameter is not supported.
<!-- end id1 -->
<!-- npu="910" id2 -->
- Atlas training products: This parameter is not supported.
<!-- end id2 -->
<!-- npu="910b" id3 -->
- Atlas A2 products: This parameter is supported.
<!-- end id3 -->
<!-- npu="950" id4 -->
- Ascend 950PR&950DT products: This parameter is not supported.
<!-- end id4 -->

## Dependencies and Constraints

Enablement of this option is usually not directly controlled by users.

At the TorchAir layer, the option corresponding to Super Kernel is recognized from the user's code or enabled manually. Only when TorchAir explicitly passes the Super Kernel enablement information and adds the Super Kernel option does the operator compilation tool add this input option.
