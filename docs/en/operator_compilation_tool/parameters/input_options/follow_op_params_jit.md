# --follow\_op\_params\_jit

<!-- md-trans-meta sourceCommit=dab20c1a5c0505cf777deac6d50259bef05c16c7 translatedAt=2026-10-30T10:04:18.850Z pushedAt=2026-11-03T00:55:22.813Z nl=2 -->

## Description

This option is optional. When it is enabled, the compilation system automatically selects either the deterministic or non-deterministic compilation mode based on the **deterministic** field in the dump operator JSON file, thereby effectively improving compilation performance. It is abbreviated as **-f**.

When the **deterministic**" field is set to **true**, the deterministic switch is turned on. When the **deterministic** field is set to **false**, the deterministic switch is turned off. This option is not configured by default. In this case, the **deterministic** field in the JSON file is not read, and two operator kernels are compiled: one with the deterministic switch on and one with the deterministic switch off.

## Related Parameters

None

## Parameter Values

None

## Recommended Configuration and Benefits

None

## Examples

```bash
op_compiler --op_params_dir=<dump_dir>  --soc_version=<soc_version>  --follow_op_params_jit
```

## Supported Models

<!-- npu="310p" id1 -->
- Atlas inference products: This parameter is supported.
<!-- end id1 -->
<!-- npu="910" id2 -->
- Atlas training products: This parameter is supported.
<!-- end id2 -->
<!-- npu="910b" id3 -->
- Atlas A2 products: This parameter is supported.
<!-- end id3 -->
<!-- npu="950" id4 -->
- Ascend 950PR&950DT products: This parameter is supported.
<!-- end id4 -->

## Dependencies and Constraints

None
