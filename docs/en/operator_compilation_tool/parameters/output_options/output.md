# --output

<!-- md-trans-meta sourceCommit=2575ba72def56b10b4e28610e3f0129db5bc2da9 translatedAt=2026-10-15T11:27:52.578Z pushedAt=2026-10-16T01:20:29.054Z -->

## Description

(Optional) Specifies the path and name of the installation package generated during compilation, for example, ***xxx*/*xxx*/*xxx*.run**. Both relative paths and absolute paths are supported. If no path is specified, the package is generated in the current directory. If no package name is specified, the static shape installation package is named `static_kernel_${datetime}_${pid}.run` by default, and the debug version installation package is named `debug_kernel_${datetime}.run` by default. This parameter is abbreviated as **-o**.

## Related Parameters

None

## Parameter Values

**Parameter value:** Path and name of the installation package generated during compilation.

**Parameter value format:** The value can be a relative path or an absolute path, and can contain uppercase and lowercase letters (a-z, A-Z), digits (0-9), underscores (\_), hyphens (-), periods (.), and Chinese characters.

**Default value:** By default, `static_kernel_${datetime}_${pid}.run` and `debug_kernel_${datetime}.run` are generated in the current directory where this tool is executed.

## Recommended Configuration and Benefits

None

## Examples

```bash
op_compiler --op_params_dir=<dump_dir>  --soc_version=Ascendxxxx --log=info --job=128 --compile_mode=tune --output=$HOME/opcompile/out/
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
