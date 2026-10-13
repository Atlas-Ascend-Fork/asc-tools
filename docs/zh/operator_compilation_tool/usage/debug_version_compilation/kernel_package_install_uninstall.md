# Kernel包安装和卸载

下文以默认命名的Kernel包为例，实际运行时，具体命令请替换为实际的Kernel包名。

- 安装调试版本的Kernel包。

    进入`debug_kernel_${datetime}.run`包所在目录，以运行用户（如HwHiAiUser）身份运行run包：

    ```bash
    ./debug_kernel_${datetime}.run
    ```

    当出现如下回显信息代表安装成功。

    ```text
    installation complete.
    ```

    目前暂不支持指定目录安装，run包默认安装到`${install_path}/opp/debug_kernel`路径下，其中`${install_path}`为CANN软件安装后文件存储路径。

    > [!NOTE]说明
    >run包默认安装路径`${install_path}/opp/debug_kernel`的默认权限为755。如果权限不足导致调试版本的Kernel包安装失败，可联系CANN软件包的安装用户修改`debug_kernel`目录权限来解决。

    run包安装后的目录结构样例如下，

    ```text
    |-- ${install_path}/opp/debug_kernel
        |-- ascendxxxx
        |   |-- gelu # 算子二进制目录
        |       |-- Gelu_5c5e58e043224cccf51a06108010ae13_high_precision.json
        |       |-- Gelu_5c5e58e043224cccf51a06108010ae13_high_precision.o
        |-- config
        |   |-- ascendxxxx
        |       |-- binary_info_config.json  # 全量Kernel包的总索引
        |       |-- gelu.json # 算子Kernel的索引
        |-- scripts # 工具涉及的通用脚本
        |   |-- common.py
        |   |-- help.info
        |   |-- install.py
        |   |-- uninstall.py
        |-- uninstall.sh  # 卸载脚本
        |-- version.info # 版本信息
    ```

    > [!NOTE]说明
    >多个Kernel包安装的情况下，后安装的Kernel包会覆盖已安装的Kernel包。

- 卸载

    进入`debug_kernel_${datetime}.run`包的安装目录，以运行用户（如HwHiAiUser）身份运行uninstall.sh。

    ```bash
    cd ${install_path}/opp/debug_kernel
    ./uninstall.sh
    ```

    卸载成功，`debug_kernel`文件夹将会被删除。
