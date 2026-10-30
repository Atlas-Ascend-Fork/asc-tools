# Ascend C Tools

简体中文 | [English](./README_en.md)

## 🚀概述

Ascend C Tools是[CANN](https://hiascend.com/software/cann) （Compute Architecture for Neural Networks）基于[Ascend C](https://gitcode.com/cann/asc-devkit)编程语言推出的配套调试工具。借助Ascend C Tools，开发者可以进行CPU域孪生调试、解析算子调测信息以及文件信息，从而快速定位算子实现中可能存在的问题。

- **cpu debug**

    cpu debug工具本质上是提供了CPU调试库文件，使得Ascend C源码可以通过通用GCC编译器编译得到在CPU上运行、调测的算子二进制文件。该工具辅助开发者在CPU上完成功能和精度的基本验证，并提供了gdb调试、printf打印等调试手段。

- **npu-check**

    npu-check是面向昇腾NPU的运行时正确性检查工具，提供内存访问检查、同步配对检查和寄存器初始化检查，帮助开发者定位算子运行中的错误。

- **npu-compute**

    npu-compute用于运行NPU应用并采集算子性能数据，包括计算与流水线利用率、内存访问和流水时间线等，生成性能报告以辅助分析性能瓶颈。

- **msobjdump**

    msobjdump针对Kernel直调算子开发与工程化算子开发编译生成的算子ELF文件（Executable and Linkable Format）提供解析、提取和Ascend AICore指令反汇编功能，并将结果信息以可读形式呈现，方便开发者直观获得Kernel文件信息。

- **show_kernel_debug_data**

    show_kernel_debug_data工具用于离线解析通过AscendC::DumpTensor/AscendC::print接口保存的Kernel侧算子调试信息。

- **tool-exception-dump**

    tool-exception-dump是面向昇腾NPU的AI Core Error系统化诊断技能（skill），基于msaicerr/asys工具管线执行与判读，以知识工作流形式指导AI助手完成场景判定、报错行定位、错误类型解码、单算子复现与证据链汇聚，输出六段式诊断报告，帮助开发者按标准路径定位算子运行中的AI Core Error根因。

## 🔍目录结构说明

本代码仓目录结构如下:

```text
├── cmake                               // Ascend C Tools构建源代码
├── cpudebug                            // Ascend C Tools cpu debug工具实现源代码
│   ├── cmake                           // Ascend C Tools cpu debug 构建源代码
│   ├── include                         // Ascend C Tools cpu debug工具实现源代码
│   ├── utils                           // Ascend C Tools cpu debug工具实现源代码
│   └── src                             // Ascend C Tools cpu debug工具实现源代码
├── docs                                // Ascend C Tools使用说明
├── examples                            // Ascend C Tools样例工程
├── libraries                           // Ascend C Tools依赖的库文件
├── npu_tools                           // Ascend C Tools NPU侧工具
│   ├── npu_check                       // Ascend C Tools npu-check实现源代码
│   └── npu_compute                     // Ascend C Tools npu-compute实现源代码
├── npuchk                              // Ascend C Tools CPU调试检查与日志分析脚本
├── scripts                             // Ascend C Tools打包脚本
├── skills                             // Ascend C Tools skills目录（AI 助手技能）
│   ├── tool-exception-dump            // Ascend C Tools tool-exception-dump实现源代码
│   ├── tool-npu-check                 // Ascend C Tools tool-npu-check实现源代码
│   ├── tool-npu-compute               // Ascend C Tools tool-npu-compute实现源代码
│   ├── tool-npu-objdump               // Ascend C Tools tool-npu-objdump实现源代码
│   └── tool-precision-debug           // Ascend C Tools tool-precision-debug实现源代码
├── tests                               // Ascend C Tools的UT用例
├── third_party                         // Ascend C Tools依赖的第三方库文件
└── utils
    ├── msobjdump                       // Ascend C Tools msobjdump实现源代码
    └── show_kernel_debug_data          // Ascend C Tools show_kernel_debug_data实现源代码
```

## 📖文档介绍

| 文档 | 说明 |
|------|------|
|[快速入门](./docs/zh/00_quick_start.md)|快速体验项目的简易教程。包括环境搭建、编译执行、本地验证等操作。|
|[使用说明](./docs)|各工具使用说明。|
|[相关文档](https://hiascend.com/document/redirect/CannCommunityOpdevAscendC)|Ascend C算子编程指南，同时该文档中提供了孪生调试cpu_debug、msobjdump、show_kernel_debug_data工具的详细介绍。|

## 📝相关信息

- [贡献指南](CONTRIBUTING.md)
- [安全声明](SECURITY.md)
- [许可证](LICENSE)
