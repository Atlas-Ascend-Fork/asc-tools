# --log

<!-- md-trans-meta sourceCommit=681b3fab3b2117829dc3976c9606bfc0f3d15012 translatedAt=2026-10-14T08:44:29.931Z pushedAt=2026-10-15T11:15:08.864Z -->

## Description

Sets the log level during operator compilation. It is abbreviated as **-l**.

- **Log persistence**

    By default, logs are persisted to the following path: ***$HOME*/ascend/log/debug/plog/plog-_pid_\_\*.log**

    **_pid_** represents the process ID, and **\*** represents the timestamp when the log file is created.

- **Log printing**

    Sets the log level. After the log function is enabled, logs are not printed by default. To print and display logs, set the following environment variable in the current window where the **op\_compiler** command is executed, and then execute the **op\_compiler** command:

    ```bash
    export ASCEND_SLOG_PRINT_TO_STDOUT=1
    ```

## Related Parameters

None

## Parameter Values

**Parameter values:**

- **debug**: Outputs runtime information at the debug/info/warning/error/event levels.
- **info**: Outputs runtime information at the info/warning/error/event levels.
- **warning**: Outputs runtime information at the warning/error/event levels.
- **error**: Outputs runtime information at the error/event levels.

**Default value**: By default, no logs are output.

## Recommended Configuration and Benefits

None

## Examples

```bash
op_compiler --op_params_dir=<dump_dir>  --soc_version=<soc_version> --log=debug
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
