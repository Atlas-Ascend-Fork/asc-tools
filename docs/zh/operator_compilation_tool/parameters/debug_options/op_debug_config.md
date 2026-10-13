# --op\_debug\_config

## 功能说明

调试配置文件的路径及文件名。

## 关联参数

无。

## 参数取值

**参数值**：配置文件路径及文件名。

**参数值格式**：路径和文件名：支持大小写字母（a-z，A-Z）、数字（0-9）、下划线（\_）、短横线（-）、句点（.）、中文汉字。

**参数值约束**：

配置文件中支持配置如下选项，多个选项使用英文逗号分隔。

- **oom**：算子**执行**过程中，检测Global Memory是否内存越界。
    - 配置该选项，算子编译时，在当前执行路径算子编译生成的kernel\_meta文件夹中保留.o（算子二进制文件）和.json文件（算子描述文件）。
    - 使用该选项后，在算子编译过程中会加入如下的检测逻辑，用户可以通过再使用**dump\_cce**参数，在生成的.cce文件中查看如下的代码。

        ```cpp
        inline __aicore__ void  CheckInvalidAccessOfDDR(xxx) {
            if (access_offset < 0 || access_offset + access_extent > ddr_size) {
                if (read_or_write == 1) {
                    trap(0X5A5A0001);
                } else {
                    trap(0X5A5A0002);
                }
            }
        }
        ```

        实际执行推理过程中，如果确实存在内存越界，会抛出“**EZ9999**”错误码。

- **dump\_bin**：算子编译时，在当前执行路径算子编译生成的kernel\_meta文件夹中保留.o（算子二进制文件）和.json文件（算子描述文件）。
- **dump\_cce**：算子编译时，在当前执行路径算子编译生成的kernel\_meta文件夹中保留算子cce文件\*.cce，以及.o（算子二进制文件）和.json文件（算子描述文件）。
- **dump\_loc**：算子编译时，在当前执行路径算子编译生成的kernel\_meta文件夹中保留python-cce映射文件\*\_loc.json。
- **ccec\_O0**：算子编译时，开启ccec编译器选项-O0，配置该选项**不会**对调试信息执行优化操作，用于后续分析AICore Error问题。
- **ccec\_g**：算子编译时，开启ccec编译器选项-g，配置该选项**会**对调试信息执行优化操作，用于后续分析AICore Error问题。
- **check\_flag**：算子**执行**时，检测算子内部流水线同步信号是否匹配。
    - 配置该选项，算子编译时，在当前执行路径算子编译生成的kernel\_meta文件夹中保留.o（算子二进制文件）和.json文件（算子描述文件）。
    - 使用该选项后，在算子编译过程中会加入如下的检测逻辑，用户可以通过再使用**dump\_cce**参数，在生成的.cce文件中查看如下的代码。

        ```cpp
          set_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
          set_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID1);
          set_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID2);
          set_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID3);
          ....
          pipe_barrier(PIPE_MTE3);
          pipe_barrier(PIPE_MTE2);
          pipe_barrier(PIPE_M);
          pipe_barrier(PIPE_V);
          pipe_barrier(PIPE_MTE1);
          pipe_barrier(PIPE_ALL);
          wait_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
          wait_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID1);
          wait_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID2);
          wait_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID3);
          ...
        ```

        实际执行推理过程中，如果确实存在算子内部流水线同步信号不匹配，则最终会在**有问题的算子处超时报错，并终止程序**，报错信息示例为：

        ```text
        Aicore kernel execute failed, ..., fault kernel_name=算子名,...
        rtStreamSynchronizeWithTimeout execute failed....
        ```

> [!NOTE]说明
>
> - 配置ccec编译选项（即ccec\_O0、ccec\_g选项）时，会导致算子Kernel（\*.o文件）大小增大。动态Shape场景下，由于算子编译时会遍历可能的Shape场景，因此可能会导致算子Kernel文件过大而无法进行编译，此种场景下，建议不要配置ccec编译选项。
>   由于算子Kernel文件过大而无法编译的报错日志示例如下：
>
>   ```text
>   message:link error ld.lld: error: InputSection too large for range extension thunk ./kernel_meta_xxxxx.o:
>   ```
>
> - ccec编译选项ccec\_O0和oom选项不可同时开启，会导致AICore Error报错，报错信息示例如下：
>
>   ```text
>   ...there is an aivec error exception, core id is 49, error code = 0x4 ...
>   ```
>
> - oom配置选项不支持和NPU\_COLLECT\_PATH环境变量同时使用，否则编译出的算子Kernel包在使用过程中会出现报错。

## 推荐配置及收益

无。

## 示例

假设使能Global Memory内存检测功能的配置文件名称为_gm\_debug.cfg_，文件内容配置示例如下：

```text
op_debug_config=ccec_g,oom
```

将该文件上传到编译工具所在服务器，例如上传到_$HOME/module_，使用示例如下：

```bash
op_compiler --kernel_name=<kernel_name>  --op_debug_config=gm_debug.cfg --soc_version=<soc_version> --log=info --job=128  --output=<output_dir>
```

## 支持的型号

<!-- npu="310p" id1 -->
- Atlas推理系列产品，支持该参数
<!-- end id1 -->
<!-- npu="910" id2 -->
- Atlas训练系列产品，支持该参数
<!-- end id2 -->
<!-- npu="910b" id3 -->
- Atlas A2系列产品，支持该参数
<!-- end id3 -->
<!-- npu="950" id4 -->
- Ascend 950PR&950DT系列产品，不支持该参数
<!-- end id4 -->

## 使用约束

无。
