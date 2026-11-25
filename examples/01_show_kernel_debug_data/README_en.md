# show_kernel_debug_data Example

## Overview

This example demonstrates the generation and parsing workflow of kernel-side debugging information based on the Add operator. The example calls `AscendC::DumpTensor`, `AscendC::printf`, and `AscendC::PrintTimeStamp` within an Ascend C kernel to generate debugging data, and then parses the dump binary files using the [show_kernel_debug_data tool](../../docs/en/04_show_kernel_debug_data.md).

## Supported Products and CANN Software Versions

| Product | CANN Software Version |
|------|-------------|
| Ascend 950PR/Ascend 950DT | >= CANN 9.1.0 |
| Atlas A3 Training Series Products/Atlas A3 Inference Series Products | >= CANN 9.0.0 |
| Atlas A2 Training Series Products/Atlas A2 Inference Series Products | >= CANN 9.0.0 |

## Directory Structure

```shell
├── 01_show_kernel_debug_data
│   ├── CMakeLists.txt      // Build project file
│   ├── acl.json            // Dump configuration file
│   ├── add.asc             // Ascend C operator implementation & invocation example
│   └── README.md           // Example documentation
```

## Example Description

- Example functionality:

  The Add computation formula is:

  ```shell
  z = x + y
  ```

- Debugging information generation:

  The example prints input/output Tensor segments on the kernel side via `AscendC::DumpTensor`, prints formatted logs via `AscendC::printf` and `AscendC::PRINTF`, and prints timestamps via `AscendC::PrintTimeStamp`. After running `./demo`, dump binary files will be generated in the `output` directory according to the `acl.json` configuration.

## Build and Run

Execute the following steps in the root directory of this example to build and run it.

- Configure environment variables

  Please configure environment variables according to the [installation method](../../docs/en/00_quick_start.md#prepare&install) of the CANN development toolkit on your current environment.

  ```bash
  source ${install_path}/cann/set_env.sh
  ```

  > **Note:** `${install_path}` is the CANN package installation directory. When no installation directory is specified, the default installation path is `/usr/local/Ascend`.

- Check tool environment

  Execute the following command. If the help information is displayed correctly, the tool environment is properly configured.

  ```bash
  show_kernel_debug_data -h
  ```

- Run the example

  Execute the following commands in this example directory.

  ```bash
  mkdir -p build output && cd build;
  cmake -DCMAKE_ASC_ARCHITECTURES=dav-2201 ..;make -j;
  ./demo
  ```

- Build option description

  | Option | Values | Description |
  |------|--------|------|
  | `CMAKE_ASC_ARCHITECTURES` | `dav-2201` (default), `dav-3510` | NPU architecture:<br>&bull; dav-2201, corresponding to Atlas A2 Training Series Products/Atlas A2 Inference Series Products and Atlas A3 Training Series Products/Atlas A3 Inference Series Products<br>&bull; dav-3510, corresponding to Ascend 950PR/Ascend 950DT |

- Execution result

  The execution result is as follows, indicating that the accuracy comparison succeeded.

  ```bash
  [Success] Case accuracy is verification passed.
  ```

  After execution completes, kernel debugging information bin files will be generated in the `output` directory, for example:

  ```text
  output
  └── 202xxxxxxxxxxx
      ├── asc_kernel_data_xxx.bin
      ├── ...
      └── asc_kernel_data_xxx.bin
  ```

## Debug Data Parsing

Execute the following commands in the `build` directory to parse the kernel-side debugging information.

```bash
mkdir -p dump_info_output
ASCEND_GLOBAL_LOG_LEVEL=1 show_kernel_debug_data ../output dump_info_output
```

The following print information can be observed in the terminal:

```bash
================ block.0 begin ==============
fmt string int: 291
fmt string int: 291
fmt string float: 3.140000
fmt string float: 3.140000
================ block.0 end ================
...
================ block.7 begin ==============
fmt string int: 291
fmt string int: 291
fmt string float: 3.140000
fmt string float: 3.140000
================ block.7 end ================
parser log saved to /path/to/build/dump_info_output/PARSER_20251022074515310995/parser.log
```

The parsed result directory structure is as follows:

```text
dump_info_output
└── PARSER_20251022074515310995
    ├── dump_data
    │   ├── 0
    │   │   ├── asc_kernel_data_aiv_0_index_0_loop_0.bin
    │   │   ├── asc_kernel_data_aiv_0_index_0_loop_0.txt
    │   │   └── time_stamp_core_0.csv
    │   ├── 1
    │   ├── ...
    │   └── index_dtype.json
    └── parser.log # The default log level is ERROR. This file is created when an error is logged. To include normal parsing logs, run export ASCEND_GLOBAL_LOG_LEVEL=1 (INFO) before running the parsing tool.
```

The directories `0`, `1`, ..., `7` under `dump_data` correspond to the print information of the 8 cores respectively. `index0`, `index1`, and `index2` correspond to the prints with the second parameter `desc=0`, `desc=1`, and `desc=2` of `DumpTensor` in the example code, namely `xLocal`, `yLocal`, and `zLocal`.
