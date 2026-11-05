# --soc\_version

<!-- md-trans-meta sourceCommit=dab20c1a5c0505cf777deac6d50259bef05c16c7 translatedAt=2026-10-30T10:06:41.318Z pushedAt=2026-11-03T00:55:22.818Z nl=2 -->

## Description

Specifies the AI processor model used during operator compilation. This parameter is required for operator compilation and is abbreviated as **-v**.

## Related Parameters

None

## Parameter Values

Model of the AI processor used for operator compilation.

If you cannot determine the model of the current AI processor, run the **npu-smi info** command on the server where the NPU driver package is installed to query it. Add **Ascend** before the queried **Name** value. For example, if the **Name** value is **xxxyy**, the actual configured **soc\_version** value is **Ascendxxxyy**.

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
- Atlas A2 products
<!-- end id3 -->
<!-- npu="950" id4 -->
- Ascend 950PR&950DT products
<!-- end id4 -->

## Dependencies and Constraints

None
