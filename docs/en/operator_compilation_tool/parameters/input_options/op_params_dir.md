# --op\_params\_dir

<!-- md-trans-meta sourceCommit=681b3fab3b2117829dc3976c9606bfc0f3d15012 translatedAt=2026-10-14T08:50:25.620Z pushedAt=2026-10-15T11:15:08.870Z -->

## Description

Specifies the path where the statistics data exported by the Dump tool is stored. Either this parameter or [--kernel\_name](kernel_name.md) must be specified. Both absolute and relative paths are supported. It is abbreviated as **-p**.

## Related Parameters

None

## Parameter Values

The value can contain uppercase and lowercase letters (a-z, A-Z), digits (0-9), underscores (\_), hyphens (-), periods (.), and Chinese characters.

## Recommended Configuration and Benefits

None

## Examples

```bash
op_compiler --op_params_dir=<dump_dir>  --soc_version=<soc_version>
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
