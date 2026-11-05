# --super\_kernel\_debug\_options

<!-- md-trans-meta sourceCommit=2e0f6e7cad32f84a05321a16885c71c448cd9b6f translatedAt=2026-10-30T10:05:30.149Z pushedAt=2026-11-03T00:55:22.815Z nl=2 -->

## Description

Sets the debug options used when compiling the SuperKernel static kernel after the Super Kernel feature is enabled.

This parameter accepts a structured dictionary string, supporting a standard JSON object or a Python dictionary literal.

## Related Parameters

- This parameter must be used together with **--enable\_super\_kernel**.
- This parameter can be used alone or together with **--super\_kernel\_optimize\_options**.
- This parameter cannot be used together with **--super\_kernel\_options**. You can only use the original merged string API, or select one or two new structured APIs.
- When this parameter is used together with **--super\_kernel\_optimize\_options**, the two dictionaries cannot contain the same top-level key.

## Parameter Values

**--super\_kernel\_debug\_options** is followed by a string after the equal sign (or space). The string content must be parseable as a dictionary, and the top-level object cannot be a list, string, number, or **null/None**. Dictionary keys must be strings, and values must strictly use the types and values supported by the corresponding SuperKernel options.

The following two formats are supported:

```bash
# Format 1 (recommended): single quotes outside, double quotes inside, with standard JSON content.
--super_kernel_debug_options='{"debug_sync_all":0}'

# Format 2: double quotes outside, single quotes inside, with Python dictionary literal content.
--super_kernel_debug_options="{'debug_sync_all':0}"
```

For the available options, see [TorchAir Usage Guide](https://gitcode.com/ascend/torchair/blob/master/docs/zh/overview.md).

## Recommended Configuration and Benefits

None

## Examples

```bash
op_compiler --op_params_dir=<dump_dir>  --soc_version=<soc_version>  --enable_super_kernel --super_kernel_debug_options='{"debug_sync_all":1,"debug_op_exec_trace":1}'
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
