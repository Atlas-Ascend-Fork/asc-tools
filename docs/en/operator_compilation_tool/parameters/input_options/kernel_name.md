# --kernel\_name

<!-- md-trans-meta sourceCommit=dab20c1a5c0505cf777deac6d50259bef05c16c7 translatedAt=2026-10-30T10:05:08.468Z pushedAt=2026-11-03T00:55:22.814Z nl=2 -->

## Description

Specifies the **kernel\_name** of the debug-version operator to be compiled. Either this parameter or [--op\_params\_dir](op_params_dir.md) is required. It is abbreviated as **-k**.

The **kernel\_name** can be obtained as follows: When an error occurs during operator execution, the **kernel\_name** of the faulty operator can be obtained from the error log (search for the **kernel\_name** keyword). For details about how to obtain the error log, see [Log Reference](https://gitcode.com/cann/runtime/blob/master/docs/zh/log_ref/README.md).

## Related Parameters

Used together with the [--op\_debug\_config](../debug_options/op_debug_config.md) parameter. This parameter can be used only when compiling the binary file of the debug version.

## Parameter Values

Search for the **kernel\_name** keyword in the operator error log to find the **kernel\_name** where the error occurred.

## Recommended Configuration and Benefits

None

## Examples

```bash
op_compiler --kernel_name=xxx  --soc_version=<soc_version>
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
