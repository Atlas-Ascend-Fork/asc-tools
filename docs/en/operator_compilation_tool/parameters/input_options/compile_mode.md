# --compile\_mode

<!-- md-trans-meta sourceCommit=2575ba72def56b10b4e28610e3f0129db5bc2da9 translatedAt=2026-10-15T11:24:49.103Z pushedAt=2026-10-16T01:20:29.048Z -->

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
- Atlas A2 products: This parameter is supported.
<!-- end id3 -->
<!-- npu="950" id4 -->
- Ascend 950PR&950DT products: This parameter is not supported.
<!-- end id4 -->

## Dependencies and Constraints

None
