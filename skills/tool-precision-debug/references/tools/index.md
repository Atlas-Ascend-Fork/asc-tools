# 工具选用

本页列出精度调试中可直接使用的 CANN 工具和调试 API，按需要选择对应入口。工具能力随 CANN 版本变化，使用 CANN 版本实际安装的工具。

## 工具与调试 API

| 工具或 API | 用途 | 使用入口 |
| --- | --- | --- |
| `DumpTensor` | 在 Kernel 内主动调用，输出指定 Tensor 的值 | [dumptensor.md](../diagnosis/dumptensor.md) |
| `show_kernel_debug_data` | 解析已采集的 Kernel Tensor、printf 等调测数据 | [kernel-debug-data.md](kernel-debug-data.md) |
| `msSanitizer` | 检查设备执行中的访存、竞争或同步问题 | [官方仓库](https://gitcode.com/Ascend/mssanitizer) |
| `msobjdump` | 查看 Kernel 元数据、提取设备 ELF，工具支持时反汇编设备指令 | [msobjdump.md](msobjdump.md) |

## 调用准备

使用工程已有的 Python 环境及实际采用的 CANN 安装，先核对相关工具帮助和输入格式。已有可用路径时直接调用；安装不明或调用失败时，查当前安装的工具目录和配套说明。环境身份在实际运行端确认，缺失字段保留未知。

工具产生的日志、比较结果、dump 和新配置放在[任务产物目录](../diagnosis/reporting.md#任务产物目录)中，每轮使用独立文件或子目录，保留原始输入。
