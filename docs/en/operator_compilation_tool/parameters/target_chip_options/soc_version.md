# --soc\_version

<!-- md-trans-meta sourceCommit=681b3fab3b2117829dc3976c9606bfc0f3d15012 translatedAt=2026-10-14T08:53:59.562Z pushedAt=2026-10-15T11:15:08.875Z -->

## Description

Specifies the AI processor model used during operator compilation. This parameter is required for operator compilation and is abbreviated as **-v**.

## Related Parameters

None

## Parameter Values

Model of the AI processor used for operator compilation.

If you cannot determine the model of the current AI processor, run the **npu-smi info** command on the server where the NPU driver package is installed to query it. Add **Ascend** before the queried **Name** value. For example, if the **Name** value is **_xxxyy_**, the actual configured **soc\_version** value is **Ascend_xxxyy_**.

## Recommended Configuration and Benefits

None

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
