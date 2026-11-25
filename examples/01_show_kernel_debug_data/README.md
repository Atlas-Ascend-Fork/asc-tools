# show_kernel_debug_data样例

## 概述

本样例基于Add算子演示kernel侧调试信息的生成和解析流程。样例在Ascend C kernel中调用`AscendC::DumpTensor`、`AscendC::printf`和`AscendC::PrintTimeStamp`生成调试数据，再通过[show_kernel_debug_data工具](../../docs/zh/04_show_kernel_debug_data.md)解析dump二进制文件。

## 本样例支持的产品及CANN软件版本

| 产品 | CANN软件版本 |
|------|-------------|
| Ascend 950PR/Ascend 950DT | >= CANN 9.1.0 |
| Atlas A3 训练系列产品/Atlas A3 推理系列产品 | >= CANN 9.0.0 |
| Atlas A2 训练系列产品/Atlas A2 推理系列产品 | >= CANN 9.0.0 |

## 目录结构介绍

```
├── 01_show_kernel_debug_data
│   ├── CMakeLists.txt      // 编译工程文件
│   ├── acl.json            // dump配置文件
│   ├── add.asc             // Ascend C算子实现 & 调用样例
│   └── README.md           // 样例说明文档
```

## 样例描述

- 样例功能：

  Add计算公式为：

  ```
  z = x + y
  ```

- 调试信息生成：

  样例在kernel侧通过`AscendC::DumpTensor`打印输入输出Tensor片段，通过`AscendC::printf`和`AscendC::PRINTF`打印格式化日志，通过`AscendC::PrintTimeStamp`打印时间戳。运行`./demo`后，dump二进制文件会按`acl.json`配置生成到`output`目录。

## 编译运行

在本样例根目录下执行如下步骤，编译并执行样例。

- 配置环境变量

  请根据当前环境上CANN开发套件包的[安装方式](../../docs/zh/00_quick_start.md#prepare&install)，配置环境变量。

  ```bash
  source ${install_path}/cann/set_env.sh
  ```

  > **说明：** `${install_path}` 为CANN包安装目录，未指定安装目录时默认安装至 `/usr/local/Ascend` 下。

- 检查工具环境

  执行如下命令，若能正常显示帮助信息，则表示工具环境正常。

  ```bash
  show_kernel_debug_data -h
  ```

- 样例执行

  在本样例目录下执行如下命令。

  ```bash
  mkdir -p build output && cd build;
  cmake -DCMAKE_ASC_ARCHITECTURES=dav-2201 ..;make -j;
  ./demo
  ```

- 编译选项说明

  | 选项 | 可选值 | 说明 |
  |------|--------|------|
  | `CMAKE_ASC_ARCHITECTURES` | `dav-2201`（默认）、`dav-3510` | NPU 架构：<br>&bull; dav-2201，对应 Atlas A2 训练系列产品/Atlas A2 推理系列产品和 Atlas A3 训练系列产品/Atlas A3 推理系列产品<br>&bull; dav-3510，对应 Ascend 950PR/Ascend 950DT |

- 执行结果

  执行结果如下，说明精度对比成功。

  ```bash
  [Success] Case accuracy is verification passed.
  ```

  执行完成后将在`output`目录下生成kernel调试信息bin文件，例如：

  ```text
  output
  └── 202xxxxxxxxxxx
      ├── asc_kernel_data_xxx.bin
      ├── ...
      └── asc_kernel_data_xxx.bin
  ```

## 调试数据解析

在`build`目录下执行如下命令，解析kernel侧调试信息。

```bash
mkdir -p dump_info_output
ASCEND_GLOBAL_LOG_LEVEL=1 show_kernel_debug_data ../output dump_info_output
```

终端可观察到如下打印信息：

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

解析结果目录结构如下：

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
    └── parser.log # 默认日志级别为 ERROR，有错误日志写入时才生成。若需查看正常解析日志，请在运行解析工具前执行 export ASCEND_GLOBAL_LOG_LEVEL=1（INFO）。
```

`dump_data`目录下的`0`、`1`、...、`7`分别对应8个核的打印信息。`index0`、`index1`、`index2`分别对应样例代码中`DumpTensor`第二个参数`desc=0`、`desc=1`、`desc=2`的打印，即`xLocal`、`yLocal`、`zLocal`。
