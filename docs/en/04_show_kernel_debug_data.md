# show_kernel_debug_data

Kernel-side operator debugging information (AscendC::DumpTensor, AscendC::printf, etc.) can be obtained after configuring Dump settings. The show_kernel_debug_data tool provides offline parsing capabilities, helping users obtain and parse debugging information (converting bin files into a readable format).

For a tool usage demonstration, refer to the [show_kernel_debug_data example](../../examples/01_show_kernel_debug_data/README.md).

## Usage

- **Command-Line Method**

    ```sh
    show_kernel_debug_data  `<bin_file_path>`  [`<output_path>`]
    ```

    | Parameter | Required/Optional | Description |
    |-------|-------|-------|
    |bin_file_path| Required | The path where kernel-side debugging information is saved. Supports bin files or directories. In directory mode, `.bin` files are recursively collected and parsed together. |
    |output_path| Optional | The path to save parsing results, e.g., `"/output_dir"`. Defaults to the current command-line working directory. The tool will automatically create the directory if it does not exist. |

- **API Method**

    show\_kernel\_debug\_data API description:

    <table>
        <tr>
            <td ><strong>Function Prototype</strong></td>
            <td colspan="2">def show_kernel_debug_data(bin_file_path: str, output_path: str = './') -> None</td>
        </tr >
        <tr>
            <td ><strong>Function Description</strong></td>
            <td colspan="2">Obtains kernel-side debugging information and parses it into readable files.</td>
        </tr >
        <tr >
            <td rowspan="2"><strong>Parameters (IN)</strong></td>
            <td>bin_file_path</td>
            <td>The path where kernel-side debugging information is saved. Supports bin files or directories. String type.</td>
        </tr>
        <tr>
            <td>output_path</td>
            <td>The path to save parsing results. String type. Defaults to the current working directory. Automatically created if the directory does not exist.</td>
        </tr>
        <tr>
            <td><strong>Parameters (OUT)</strong></td>
            <td>NA</td>
                <td>-</td>
        </tr>
        <tr>
                <td><strong>Return Value</strong></td>
            <td>NA</td>
                <td>-</td>
        </tr>
        <tr>
            <td ><strong>Constraints</strong></td>
            <td colspan="2">None</td>
        </tr >
        <tr>
            <td ><strong>Call Example</strong></td>
            <td colspan="2">from show_kernel_debug_data import show_kernel_debug_data<br>
            show_kernel_debug_data("./input/dump_workspace.bin")</td>
        </tr >
    </table>

- Dump Configuration

  Enable Dump configuration through the [aclInit API](https://www.hiascend.com/document/detail/zh/canncommercial/850/API/appdevgapi/aclcppdevg_03_0022.html). Configure the dump_path parameter to set the path for saving Dump data, and configure the dump_kernel_data parameter to enable the Dump function. An example configuration file is as follows:

    ```json
    {
        "dump" :{
            "dump_kernel_data" :"all",
            "dump_path" :"../output"
        }
    }
    ```

    - dump_kernel_data: Specifies the type of data to export. Multiple types can be configured, separated by commas. The following types are currently supported:
        - all: Export output data from all of the following debugging types.
        - printf: Export output data from AscendC::printf/PRINTF debugging.
        - tensor: Export output data from AscendC::DumpTensor debugging.
        - assert: Export output data from ascend_assert debugging.
        - timestamp: Export timestamp information obtained via AscendC::PrintTimestamp.
    - dump_path: When enabling the operator Kernel debugging information Dump function, dump_path must be configured. Both absolute and relative paths are supported.

    In addition to the above method, you can also configure the Dump storage path through the environment variables ASCEND_DUMP_PATH and ASCEND_WORK_PATH. The priority of Dump file storage paths is as follows: ASCEND_DUMP_PATH > ASCEND_WORK_PATH > dump_path in the configuration file.
