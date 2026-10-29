# npu-check

## Overview

npu-check is a runtime correctness checking tool for Ascend NPUs. It helps users check operator correctness during development. Run an application with npu-check to view error types, locations, and check results.

This guide is intended for Ascend C operator developers. It covers environment setup, command-line usage, checking features, report reading, and usage examples.

When using this tool through an Agent, refer to the [tool-npu-check Skill](../../skills/tool-npu-check/SKILL.md) for applicability checks, checker selection, controlled execution, report interpretation, and operator retesting.

### Checking Features

| Feature | Purpose | Issues Checked |
| --- | --- | --- |
| `memcheck` | Checks out-of-bounds access issues | Out-of-bounds accesses in an operator, such as accessing unallocated GM or an address beyond the buffer size in the AI Core |
| `synccheck` | Checks whether device synchronization operations are correctly paired | Repeated event notifications, waits for events that have not been notified, notifications without corresponding waits, and mismatched notification/wait pairs |
| `initcheck` | Checks whether persistent registers are set before their first dependent use | Data-transfer or compute instructions using persistent registers before they are set, or registers set only after use |
| `racecheck` | Checks conflicting memory accesses that lack required synchronization | Read-after-write, write-after-read, and write-after-write races and atomic write semantic conflicts |

The four features can be used separately or enabled together for the same application run.

### Usage Scenarios

Typical uses of npu-check include:

- Checking normal inputs, boundary inputs, and tail-block handling after developing an operator.
- Checking memory accesses after changing tiling, copy lengths, or strides.
- Checking synchronization after modifying inter-pipeline synchronization logic.
- Investigating intermittent incorrect results or device execution errors.

Correct numerical results do not necessarily mean the code is correct. For example, a program may read beyond an allocation while still producing correct output because the extra data does not contribute to the final computation.

## Environment Preparation

Follow the [Quick Start](00_quick_start.md) to prepare the environment. Before using npu-check, install a CANN package compatible with the target NPU and driver, then run the following command to set the environment variables:

```bash
source ${INSTALL_DIR}/set_env.sh
```

> Replace `${INSTALL_DIR}` with the path where the CANN package is installed. When the package is installed as the root user, the default path is `/usr/local/Ascend/cann`.

### Source Location Information

To locate source files and line numbers directly from a report, preserve line information when compiling the operator.

## Command Format

```text
npu-check [--tool <checktype>]... [--log-file <filepath>] [--] <application> [args...]
```

### Options

| Option | Input | Default Behavior | Description |
| --- | --- | --- | --- |
| `--tool <checktype>` | `memcheck`, `synccheck`, `initcheck`, `racecheck` | Enables `memcheck` when omitted | Can be specified multiple times to enable multiple tools; repeating the same tool does not repeat the check |
| `--log-file <filepath>` | A file path | Displays the report in the terminal | Saves diagnostic information |
| `-h`, `--help` | None | Does not display help | Displays command help |
| `--` | None | Optional | Separates tool options from application arguments |

## Usage Examples

Run a specific type of check, replacing `<checktype>` with `memcheck`, `synccheck`, `initcheck`, or `racecheck`:

```text
npu-check --tool <checktype> ./my_app
```

When no feature is specified, memory access checking (`memcheck`) is enabled by default:

```bash
npu-check ./my_app
```

Enable all checks:

```bash
npu-check --tool memcheck --tool synccheck --tool initcheck --tool racecheck ./my_app
```

### Specifying the Report Location

Save to a specified file:

```bash
mkdir -p reports
npu-check --tool memcheck --log-file reports/memcheck.log ./my_app
```

When `--log-file` is specified, the report is written to the file and is not also displayed in the terminal. The application's console output is not affected.

## Constraints

- Device checking currently covers Ascend 950PR&950DT series products.
- A single invocation of a single operator through either `<<<>>>` or ACLNN is currently supported.
- `memcheck` checks out-of-bounds GM accesses from supported instructions and accesses beyond buffer boundaries in L1, L0A, L0B, and L0C.
- `synccheck` checks intra-core and inter-core synchronization pairing issues and repeated synchronization setting issues.
- `initcheck` checks initialization of persistent registers required by data-transfer and compute instructions, as well as initialization of GM, L1, L0A, L0B, and L0C memory.
- `racecheck` checks conflicting accesses to GM, L1, L0A, L0B, and L0C.
- `racecheck` supports ADD/MIN/MAX atomic write checking. Writes using the same atomic mode are not considered race conflicts; writes using different atomic modes report an atomic write semantic conflict when their address ranges overlap and required synchronization is missing. Atomic writes are also checked for conflicts with ordinary reads and writes.
- Writes with atomic mode disabled are checked as ordinary writes. Unsupported or unrecognized atomic configurations make race analysis incomplete.
- `racecheck` can report multiple races within a single launch and continues checking other memory ranges after finding the first race. Only the first confirmed race for a given memory range is reported; subsequent races in that range are not reported again.
- The target program must be able to run independently without `npu-check`.
- The target program must be an executable file.
- Multi-process and multi-threaded execution are not supported.
- `racecheck` and `initcheck` are not supported for `simt vf` and `simd vf` scenarios.
- In `simt vf` and `simd vf` scenarios, `memcheck` only checks out-of-bounds reads and writes generated by non-`vf` code.
- Operators that use non-`inline` child functions are not supported.
- For `initcheck` memory initialization issues that are also race conditions, such as read-after-write cases, the tool only guarantees no false positives. Existing race conditions are reported as expected.
- Synchronization pairing checks in `synccheck`, as well as diagnostics that depend on synchronization relationships, such as `initcheck` and `racecheck`, support hardware synchronization only. If synchronization is implemented through software logic, the related diagnostic results are unreliable. The following hardware synchronization APIs are supported on Ascend 950PR&950DT series products:

  | Synchronization Scope | Ascend C Basic APIs | Corresponding C APIs |
  | --- | --- | --- |
  | Intra-core pipeline events | `SetFlag/WaitFlag`, `TQueSync::SetFlag/WaitFlag` | `asc_sync_notify/asc_sync_wait` |
  | Intra-core pipeline locks | `Mutex::Lock/Unlock` | `asc_lock/asc_unlock` |
  | Inter-core mode 0: all AICs or all AIVs | `CrossCoreSetFlag<0, pipe>/CrossCoreWaitFlag<0, pipe>` | `asc_sync_inter_arrive/asc_sync_inter_wait` |
  | Inter-core mode 1: all AIVs in one group | `CrossCoreSetFlag<1, pipe>/CrossCoreWaitFlag<1, pipe>` | `asc_sync_subblock_arrive/asc_sync_subblock_wait` |
  | Inter-core mode 2: AIC and all AIVs in one group | `CrossCoreSetFlag<2, pipe>/CrossCoreWaitFlag<2, pipe>` | `asc_sync_block_arrive/asc_sync_block_wait` |
  | Inter-core mode 4: AIC and one AIV in one group | `CrossCoreSetFlag<4, pipe>/CrossCoreWaitFlag<4, pipe>` | `asc_sync_intra_arrive/asc_sync_intra_wait` |
  | Hardware synchronization across all cores | `SyncAll<isAIVOnly>()` and `SyncAll<isAIVOnly, config>()` without workspace arguments | None |

  If diagnostics indicate mismatched synchronization operations, initialization issues, or race conditions within a high-level API, the diagnostic results reported for this run are unreliable.
