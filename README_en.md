# Ascend C Tools

## 🚀 Overview

Ascend C Tools is a debugging toolkit provided by [CANN](https://hiascend.com/software/cann) (Compute Architecture for Neural Networks) based on the [Ascend C](https://gitcode.com/cann/asc-devkit) programming language. With Ascend C Tools, developers can perform CPU-domain twin debugging, parse operator debugging information and file information, thereby quickly locating potential issues in operator implementations.

- **cpu debug**

    The cpu debug tool essentially provides a CPU debugging library that enables Ascend C source code to be compiled with a standard GCC compiler into operator binaries that can run and be debugged on the CPU. This tool assists developers in performing basic functional and accuracy verification on the CPU, and provides debugging methods such as GDB debugging and printf output.

- **npu-check**

    npu-check is a runtime correctness checking tool for Ascend NPUs. It checks memory accesses, synchronization pairing, and register initialization to help developers locate errors during operator execution.

- **npu-compute**

    npu-compute runs NPU applications and collects operator performance data, including compute and pipeline utilization, memory accesses, and pipeline timelines. It generates performance reports to help analyze performance bottlenecks.

- **msobjdump**

    msobjdump provides parsing, extraction, and Ascend AICore instruction disassembly capabilities for operator ELF (Executable and Linkable Format) files compiled from Kernel direct-invocation operator development and engineered operator development, presenting the resulting information in a readable format so that developers can intuitively obtain Kernel file information.

- **show_kernel_debug_data**

    The show_kernel_debug_data tool is used for offline parsing of Kernel-side operator debugging information saved via the AscendC::DumpTensor/AscendC::print interfaces.

- **tool-exception-dump**

    tool-exception-dump is a systematic AI Core Error diagnosis skill for Ascend NPUs. Built on the msaicerr/asys tool pipeline, it guides AI assistants through scenario classification, error line location, error type decoding, single-operator reproduction, and evidence-chain convergence with fix suggestions, producing a six-section diagnosis report to help developers locate AI Core Error root causes along a standard path.

## 🔍 Directory Structure

The directory structure of this repository is as follows:

```text
├── cmake                               // Ascend C Tools build source code
├── cpudebug                            // Ascend C Tools cpu debug tool implementation source code
│   ├── cmake                           // Ascend C Tools cpu debug build source code
│   ├── include                         // Ascend C Tools cpu debug tool implementation source code
│   ├── utils                           // Ascend C Tools cpu debug tool implementation source code
│   └── src                             // Ascend C Tools cpu debug tool implementation source code
├── docs                                // Ascend C Tools usage documentation
├── examples                            // Ascend C Tools example projects
├── libraries                           // Library files that Ascend C Tools depends on
├── npu_tools                           // Ascend C Tools NPU-side tools
│   ├── npu_check                       // Ascend C Tools npu-check implementation source code
│   └── npu_compute                     // Ascend C Tools npu-compute implementation source code
├── npuchk                              // Ascend C Tools CPU debug checks and log analysis script
├── scripts                             // Ascend C Tools packaging scripts
├── skills                             // Ascend C Tools skills directory
│   ├── tool-exception-dump            // Ascend C Tools tool-exception-dump实现源代码
│   ├── tool-npu-check                 // Ascend C Tools tool-npu-check实现源代码
│   ├── tool-npu-compute               // Ascend C Tools tool-npu-compute实现源代码
│   ├── tool-npu-objdump               // Ascend C Tools tool-npu-objdump实现源代码
│   └── tool-precision-debug           // Ascend C Tools tool-precision-debug实现源代码
├── tests                               // Ascend C Tools UT test cases
├── third_party                         // Third-party library files that Ascend C Tools depends on
└── utils
    ├── msobjdump                       // Ascend C Tools msobjdump implementation source code
    └── show_kernel_debug_data          // Ascend C Tools show_kernel_debug_data implementation source code
```

## 📖 Documentation

| Document | Description |
|------|------|
|[Quick Start](./docs/en/00_quick_start.md)|A quick tutorial to get started with the project. Includes environment setup, compilation and execution, and local verification.|
|[Usage Guide](./docs)|Usage instructions for each tool.|
|[Related Documentation](https://hiascend.com/document/redirect/CannCommunityOpdevAscendC)|Ascend C operator programming guide, which also provides detailed introductions to the twin debugging cpu_debug, msobjdump, and show_kernel_debug_data tools.|

## 📝 Related Information

- [Contribution Guide](CONTRIBUTING.md)
- [Security Statement](SECURITY.md)
- [License](LICENSE)
