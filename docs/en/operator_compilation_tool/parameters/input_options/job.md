# --job

<!-- md-trans-meta sourceCommit=2575ba72def56b10b4e28610e3f0129db5bc2da9 translatedAt=2026-10-15T11:25:53.084Z pushedAt=2026-10-16T01:20:29.050Z -->

## Description

Sets the number of worker processes during compilation. The minimum value is **1**, and the default value is **16**. It is abbreviated as **-j**.

## Related Parameters

None

## Parameter Values

Sets the number of worker processes during compilation.

## Recommended Configuration and Benefits

None

## Examples

```bash
op_compiler --op_params_dir=<dump_dir>  --soc_version=<soc_version> --job=128
```

## Supported Models

<!-- npu="310p" id1 -->
- Atlas inference products
<!-- end id1 -->
<!-- npu="910" id2 -->
- Atlas training products
<!-- end id2 -->
<!-- npu="910b" id3 -->
- Atlas A2 products
<!-- end id3 -->
<!-- npu="950" id4 -->
- Ascend 950PR&950DT products
<!-- end id4 -->

## Dependencies and Constraints

None
