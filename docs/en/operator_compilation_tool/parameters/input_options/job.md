# --job

<!-- md-trans-meta sourceCommit=dab20c1a5c0505cf777deac6d50259bef05c16c7 translatedAt=2026-10-30T10:04:39.639Z pushedAt=2026-11-03T00:55:22.813Z nl=2 -->

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
