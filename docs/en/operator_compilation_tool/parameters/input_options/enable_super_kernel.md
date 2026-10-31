# --enable\_super\_kernel

<!-- md-trans-meta sourceCommit=2575ba72def56b10b4e28610e3f0129db5bc2da9 translatedAt=2026-10-15T11:25:20.584Z pushedAt=2026-10-16T01:20:29.049Z -->

## Description

Whether to enable the Super Kernel feature. By default, Super Kernel is disabled. Adding this option modifies the compilation behavior and uses the Super Kernel process.

## Related Parameters

None

## Parameter Values

None

## Recommended Configuration and Benefits

None

## Examples

```bash
op_compiler --op_params_dir=<dump_dir>  --soc_version=<soc_version>  --enable_super_kernel
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

The TorchAir layer identifies the user's code or manually enables the option corresponding to Super Kernel. Only when TorchAir explicitly passes the Super Kernel enablement information does **op\_compiler** add this input option.
