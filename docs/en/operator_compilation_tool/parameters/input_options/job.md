# --job

<!-- md-trans-meta sourceCommit=681b3fab3b2117829dc3976c9606bfc0f3d15012 translatedAt=2026-10-14T08:49:26.810Z pushedAt=2026-10-15T11:15:08.869Z -->

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
- Atlas A2 training products/Atlas A2 inference products
<!-- end id3 -->
<!-- npu="950" id4 -->
- Ascend 950PR/Ascend 950DT
<!-- end id4 -->

## Dependencies and Constraints

None
