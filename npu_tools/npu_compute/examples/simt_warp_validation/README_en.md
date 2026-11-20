# SIMT Warp Timeline Validation Examples

These examples share `simt_cases.asc`. Each executable launches one kernel and compares its output with a CPU reference. Use the examples to verify `occupancy` collection and report export. See the [npu-compute user guide](../../../../docs/en/06_npu_compute.md#simt-warp-occupancy).

| Case | Logic | Focus |

Cases 01 through 09 use 1024 elements. Case 10 uses 513 elements to cover a tail block. The default device ID is 0.

## Build and Run

Run these commands in this directory. Set `INSTALL_DIR` to the CANN installation that matches the device. This example uses `dav-3510`.

```bash
source "${INSTALL_DIR}/set_env.sh"
cmake -S . -B build -DNPU_ARCH=dav-3510
cmake --build build -j 4
./build/simt_case_01

export NPU_COMPUTE_NPUBIT_PLATFORM=V100
bash run_cases.sh "$(command -v npu-compute)" 01
```

Use `all` in place of `01` to run all cases. To use a tool built from source, pass the absolute path to the `npu-compute` executable.

The script creates a separate results directory under `TMPDIR` and defaults to `/tmp`. For each case, it checks the baseline run before collecting and exporting the report. It stops at the first failure. The terminal prints the results directory, which contains each case's logs, `result.npu-rep`, and `extracted` directory. Open the extracted `SimtWarpOccupancy.json` in Chrome Trace.
