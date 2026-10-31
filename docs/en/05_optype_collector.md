# optype_collector

## Overview

`optype_collector` is used to collect OpType information for a specified SoC from CANN OPP packages, and to detect OpType naming conflicts between custom operator packages and built-in operator packages, as well as between different custom operator packages. This helps developers identify naming conflicts before installing or delivering custom operators.

The tool supports the following information sources:

- CANN built-in operator packages under `<CANN installation directory>/cann/opp/built-in`.
- Custom operator packages under `<CANN installation directory>/cann/opp/vendors`.
- One or more custom operator package paths specified by `ASCEND_CUSTOM_OPP_PATH`.

## Prerequisites

The tool is installed with the asc-tools package. After installation and CANN environment variable configuration, users can invoke it directly via the `optype_collector` command without navigating to the tool directory.

## Environment Variables

CANN environment variables must be configured before use:

```bash
source <CANN installation directory>/cann/set_env.sh
```

The tool depends on the following environment variables:

| Environment Variable | Description |
| ---- | ---- |
| `ASCEND_CUSTOM_OPP_PATH` | Custom operator package path. Optional. If not set, `<CANN installation directory>/cann/opp/vendors` will still be scanned. Multiple paths are separated by the system path separator. |

## Command Format

`{soc_version}` is the target SoC name, which can be any CANN-supported product SoC name.

Existing scripts using all-lowercase SoC names remain functional.

- Output built-in operator OpType list:

  ```bash
  optype_collector {soc_version}
  optype_collector {soc_version} --builtin
  ```

- Output custom operator OpType list:

  ```bash
  optype_collector {soc_version} --custom
  ```

- Output the complete OpType list of both built-in and custom operators:

  ```bash
  optype_collector {soc_version} --all
  ```

- Detect OpType naming conflicts:

  ```bash
  optype_collector --detect-conflicts {soc_version}
  ```

## Return Values

| Return Value | Description |
| ---- | ---- |
| `0` | Execution succeeded, and no naming conflicts were found in conflict detection mode. |
| `1` | OpType naming conflicts were detected in conflict detection mode. |
| `2` | Blocking errors such as invalid parameters, missing environment variables, or missing target SoC packages. |

## Output Description

Each execution of the tool outputs the following information:

- Scan information: Displays the input SoC, `ASCEND_HOME_PATH`, and `ASCEND_CUSTOM_OPP_PATH`.
- Information sources: Displays the source type, status, number of configuration files, number of OpTypes, input SoC, and path for each matched source.
- OpType list or detection results: Outputs the OpType list or conflict groups based on the command mode.
- Warnings and errors: Information such as `ASCEND_CUSTOM_OPP_PATH` not being set or no custom operator packages being found will be output collectively after the scan completes.

When the input SoC is not supported, the tool returns a blocking error with the following prompt:

```text
Error: SoC version is not supported: {soc_version}
```

The conflict detection mode covers the following two types of conflicts:

1. OpType naming conflicts between custom operator packages and CANN built-in operator packages.
2. OpType naming conflicts between different custom operator packages.

## Examples

```bash
# View built-in operator OpTypes for {soc_version}
optype_collector {soc_version} --builtin

# View custom operator OpTypes for {soc_version}
optype_collector {soc_version} --custom

# Detect naming conflicts between custom and built-in operators, and between custom operators for {soc_version}
optype_collector --detect-conflicts {soc_version}
```
