# show_kernel_debug_data

kernel侧算子调试信息（AscendC::DumpTensor, AscendC::printf等）可通过Dump配置后进行获取。show_kernel_debug_data工具提供了离线解析能力，帮助用户获取并解析调试信息（将bin文件解析成可读格式）。

工具调用演示可参考[show_kernel_debug_data样例](../../examples/01_show_kernel_debug_data/README.md)。

## 使用方法

- **命令行方式**

    ```
    show_kernel_debug_data  `<bin_file_path>`  [`<output_path>`]
    ```

    | 参数 | 可选/必选 | 说明 |
    |-------|-------|-------|
    |bin_file_path| 必选 | kernel侧调试信息落盘路径，支持bin文件或目录。目录模式下会递归收集`.bin`文件并统一解析。 |
    |output_path| 可选 | 解析结果保存路径，例如`"/output_dir"`。默认是当前命令行执行目录。若目录不存在，工具会自动创建。 |

- **API方式**

    show\_kernel\_debug\_data接口说明：

    <table>
        <tr>
            <td ><strong>函数原型</strong></td>
            <td colspan="2">def show_kernel_debug_data(bin_file_path: str, output_path: str = './') -> None</td>
        </tr >
        <tr>
            <td ><strong>函数功能</strong></td>
            <td colspan="2">获取kernel侧调试信息并解析成可读文件。</td>
        </tr >
        <tr >
            <td rowspan="2"><strong>参数（IN）</strong></td>
            <td>bin_file_path</td>
            <td>kernel侧调试信息落盘路径，支持bin文件或目录，字符串类型。</td>
        </tr>
        <tr>
            <td>output_path</td>
            <td>解析结果保存路径，字符串类型。默认是当前工作目录，目录不存在时自动创建。</td>
        </tr>
        <tr>
            <td><strong>参数（OUT）</strong></td>
            <td>NA</td>
                <td>-</td>
        </tr>
        <tr>
                <td><strong>返回值</strong></td>
            <td>NA</td>
                <td>-</td>
        </tr>
        <tr>
            <td ><strong>使用约束</strong></td>
            <td colspan="2">无</td>
        </tr >
        <tr>
            <td ><strong>调用示例</strong></td>
            <td colspan="2">from show_kernel_debug_data import show_kernel_debug_data<br>
            show_kernel_debug_data("./input/dump_workspace.bin")</td>
        </tr >
    </table>

- Dump配置

  通过[aclInit接口](https://www.hiascend.com/document/detail/zh/canncommercial/850/API/appdevgapi/aclcppdevg_03_0022.html)启用Dump配置，需配置dump_path参数设置保存Dump数据的路径，配置dump_kernel_data参数开启Dump功能，配置文件内容的示例如下：
    ```json
    {
        "dump" :{
            "dump_kernel_data" :"all",
            "dump_path" :"../output"
        }
    }
    ```
    - dump_kernel_data：指定导出数据的类型，支持配置多个类型，用英文逗号隔开。当前支持如下类型：
        - all：导出以下所有类型调测的输出数据。
        - printf: 导出AscendC::printf/PRINTF调测的输出数据。
        - tensor: 导出AscendC::DumpTensor调测的输出数据。
        - assert: 导出ascend_assert调测的输出数据。
        - timestamp: 导出AscendC::PrintTimeStamp获取的时间戳信息。
    - dump_path：启用算子Kernel调测信息Dump功能时，dump_path必须配置，支持配置绝对路径或相对路径。

    除上述方式之外，还可以通过环境变量ASCEND_DUMP_PATH和ASCEND_WORK_PATH配置Dump存储路径。Dump文件存储路径的优先级如下：ASCEND_DUMP_PATH > ASCEND_WORK_PATH > 配置文件中的dump_path。
