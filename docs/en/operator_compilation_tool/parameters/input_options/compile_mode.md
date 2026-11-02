# --compile\_mode

<!-- md-trans-meta sourceCommit=681b3fab3b2117829dc3976c9606bfc0f3d15012 translatedAt=2026-10-14T08:46:12.374Z pushedAt=2026-10-15T11:15:08.866Z -->

## Description

(Optional) Working mode. The compilation-only mode is used by default. Setting this parameter to **tune** enables the tuning mode. The parameter is abbreviated as **-m**. The tuning function has been deprecated since CANN 9.1.0. After this parameter is set to **tune**, the tuning mode does not take effect and the default compilation process is automatically used.

## Related Parameters

None

## Parameter Values

The value can be **tune**, which is deprecated since CANN 9.1.0. After this parameter is set to **tune**, the tuning mode does not take effect.

## Recommended Configuration and Benefits

None

## Examples

None

## Supported Models

<!-- npu="310p" id1 -->
- Atlas inference products: This parameter is not supported.
<!-- end id1 -->
<!-- npu="910" id2 -->
- Atlas training products: This parameter is not supported.
<!-- end id2 -->
<!-- npu="910b" id3 -->
- Atlas A2 training products/Atlas A2 inference products: This parameter is supported.
<!-- end id3 -->
<!-- npu="950" id4 -->
- Ascend 950PR/Ascend 950DT: This parameter is not supported.
<!-- end id4 -->

## Dependencies and Constraints

None
