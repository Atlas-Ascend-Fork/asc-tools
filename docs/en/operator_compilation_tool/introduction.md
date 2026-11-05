# Introduction

<!-- md-trans-meta sourceCommit=dab20c1a5c0505cf777deac6d50259bef05c16c7 translatedAt=2026-10-30T10:01:56.430Z pushedAt=2026-11-03T00:55:22.809Z nl=2 -->

The operator compilation tool (op\_compiler) is a command-line tool provided by Ascend CANN for compiling operators and generating operator binary files. The specific functionality and usage constraints of this tool are as follows:

**Specific functionality**:

- **Static shape compilation**: When operators are called through single-operator APIs and the operator shapes are fixed, you can use this tool to compile and install static kernel packages to improve operator call performance.

    This document only describes how to use the operator compilation tool. For specific scenario examples, see the [Using Static Kernels to Improve Operator Execution Performance](https://gitcode.com/cann/docs/blob/master/docs/zh/ops-lib/appendix/performance_improve_by_staticKernel.md) section in the [Operator Library](https://gitcode.com/cann/docs/blob/master/docs/zh/ops-lib/0_README.md).

- **Debug version compilation**. When locating operator issues, you can use this tool to compile a debug version of the binary file for issue reproduction and location.

**Usage constraints**:

- **Static shape compilation**

    Supported product models:

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

- **Debug version compilation**

    Supported product models:

    <!-- npu="310p" id5 -->
    - Atlas inference products: Supported
    <!-- end id5 -->
    <!-- npu="910" id6 -->
    - Atlas training products: Supported
    <!-- end id6 -->
    <!-- npu="910b" id7 -->
    - Atlas A2 products: Supported
    <!-- end id7 -->
    <!-- npu="950" id8 -->
    - Ascend 950PR&950DT products: Not supported
    <!-- end id8 -->

## Functional Architecture of the Tool

The following figure shows the functional architecture of the operator compilation tool.

**Figure 1** Functional architecture of the operator compilation tool

![](figures/operator_compilation_tool_architecture.png "Operator compilation tool architecture")

1. When running this tool, the developer needs to pass an operator information statistics file or the operator **kernel\_name** (passing **kernel\_name** is supported only in the debug scenario) and other parameters to the tool.
2. In the scenario where an operator information statistics file is passed, the operator compilation tool obtains and parses the file.
3. Check whether the developer has enabled the debug version compilation option:
    - If enabled, compile the corresponding debug version based on whether static shape or dynamic shape is used at runtime.
    - If not enabled, perform the static shape compilation process.

4. The operator compilation tool compiles and generates the kernel file and packages it into an operator kernel package.

## Static Compilation

Static compilation means that the shape size is specified at compilation time and does not need to be specified at runtime. When the operator compilation tool performs static compilation, it obtains the determined shape information based on the input operator information statistics file, and compiles an operator binary file for each shape. The compilation principle is shown in the following figure:

![](figures/usage_scenario_1.png)

Static compilation has the following advantages:

- The sizes of all tensors are determined at compilation time, resulting in high storage space utilization.
- Targeted optimization can be performed for the actual shape size at compilation time.
- AI processors are good at running parallel instructions but not at logical computation. Too many scalar operations may interrupt the running of parallel instructions, thereby degrading performance. Static compilation can complete scalar computation at compilation time, which improves performance to a certain extent.
- The compilation tool knows the exact operation data size at compilation time, so it does not insert additional synchronization, which prevents parallel execution of multiple instructions from becoming serial execution and improves performance to a certain extent.

## Debug Version Compilation

During the use of an operator binary file, if issues such as AI Core errors or memory out-of-bounds errors occur, you can compile a debug version that contains relevant debugging information to analyze and locate the issues.
