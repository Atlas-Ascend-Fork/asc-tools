# SIMT Warp时间线验证样例

这组样例共用`simt_cases.asc`。每个可执行文件启动一次Kernel，并将输出与CPU参考结果比较。样例用于验证`occupancy`采集和报告导出。功能说明见[npu-compute用户指南](../../../../docs/zh/06_npu_compute.md#simt-warp-occupancy)。

| 编号 | 逻辑 | 验证侧重点 |

01至09使用1024个元素，10使用513个元素，以覆盖尾块。设备编号默认为0。

## 编译和运行

在本目录执行以下命令。将`INSTALL_DIR`设为匹配设备的CANN安装目录。下例为`dav-3510`。

```bash
source "${INSTALL_DIR}/set_env.sh"
cmake -S . -B build -DNPU_ARCH=dav-3510
cmake --build build -j 4
./build/simt_case_01

export NPU_COMPUTE_NPUBIT_PLATFORM=V100
bash run_cases.sh "$(command -v npu-compute)" 01
```

用`all`代替`01`可验证全部样例。如果使用源码构建的工具，传入该`npu-compute`可执行文件的绝对路径。

脚本在`TMPDIR`指定目录下创建独立结果目录，未设置时使用`/tmp`。每个样例先校验原始运行，再采集和导出报告。脚本在第一项失败时停止。终端会打印结果目录，其中包含每个样例的日志、`result.npu-rep`和`extracted`目录。用Chrome Trace打开导出的`SimtWarpOccupancy.json`。
