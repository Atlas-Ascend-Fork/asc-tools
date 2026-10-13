# Kernel包安装和卸载

下文以默认命名的Kernel包为例，实际运行时，具体命令请替换为实际的Kernel包名称。

1. 安装静态Kernel包。

    进入`static_kernel_${datetime}_${pid}.run`包所在目录，以运行用户（如HwHiAiUser）身份运行run包：

    ```bash
    ./static_kernel_${datetime}_${pid}.run
    ```

    当出现如下回显信息代表安装成功。

    ```text
    Verifying archive integrity...  100%   SHA256 checksums are OK. All good.
    Uncompressing STATIC KERNEL RUN PACKAGE  100%
    ```

    支持默认安装和指定目录安装。

    - 默认安装：run包默认安装到`${INSTALL_DIR}/opp/static_kernel`路径下，其中`${INSTALL_DIR}`为CANN软件安装后文件存储路径，请根据实际情况source CANN软件环境变量再执行默认安装

    - 指定目录安装：支持通过参数--install-path=<path\>自定义安装目标路径。

    > [!NOTE]说明
    >- run包默认安装路径`${INSTALL_DIR}/opp/static_kernel`的默认权限为770（本用户、同组用户访问）。如果权限不足导致静态Kernel包安装失败，可联系CANN软件包的安装用户修改`static_kernel`目录权限来解决。
    >- run包支持静默安装，可通过参数--quiet（简写-q）启用。

    run包安装后的目录结构样例如下，

    ```text
    |-- ${install_path}/opp/static_kernel
        |-- ai_core
            |-- config
               |-- ascendxxxx
               |    |-- binary_info_config.json            # 全量静态Kernel包的总索引
               |    |-- shape_info                         # 原始shape文件目录
               |-- static_kernel.lock                      # 文件锁
            |-- config.ini                               # 记录安装顺序的配置文件。
            |-- static_kernel_250924103634186357_988879            # 时间戳为“250924103634186357”且进程号为"988879"的静态Kernel文件
               |-- ascendxxxx
               |   |-- Add                                 # 算子二进制目录
               |      |-- static_kernel_Add_float16_NCL_xxxx_d0.json     # d0表示确定性开关关闭
               |      |-- static_kernel_Add_float16_NCL_xxxx_d1.json     # d1表示确定性开关开启
               |      |-- static_kernel_Add_float16_NCL_xxxx_d0.o
               |      |-- static_kernel_Add_float16_NCL_xxxx_d1.o
               |   |-- xxxx
               |      |-- static_kernel_xxx.json
               |      |-- static_kernel_xxx.o
               |   |-- ......
               |-- config                                # 单个静态Kernel包索引
               |   |-- ascendxxxx
               |       |-- binary_info_config.json
               |       |-- shape_info                    # 原始shape文件目录
               |-- scripts                               # 工具涉及的通用脚本
               |   |-- ......
               |-- uninstall.sh                        # 单包卸载脚本
            |-- static_kernel_xxxx                    # 不同时间戳的静态Kernel文件
            |-- uninstall.sh                           # 全量卸载脚本
            |-- version.info                             # 版本信息
    ```

    > [!NOTE]说明
    >支持多个Kernel包安装，如果多个包中存在相同的算子Kernel，以后安装的Kernel包为准。

2. （可选）当不再需要静态Kernel包时，可以单包卸载或全量卸载。
    - 单包卸载

        进入`static_kernel_${datetime}_${pid}.run`包的安装目录，以运行用户（如HwHiAiUser）身份运行uninstall.sh。

        ```bash
        cd ${install_path}/opp/static_kernel/ai_core/static_kernel_${datetime}_${pid}
        ./uninstall.sh
        ```

        卸载成功，`ai_core`目录下`static_kernel_${datetime}_${pid}`文件夹将会被删除。

    - 全量卸载

        进入`${install_path}/opp/static_kernel/ai_core`目录下，以运行用户（如HwHiAiUser）身份运行uninstall.sh。

        ```bash
        cd ${install_path}/opp/static_kernel/ai_core/
        ./uninstall.sh
        ```

        卸载成功，`ai_core`目录下除`config/static_kernel.lock`文件外，其他所有内容将会被删除，所有已安装的Kernel包均被卸载。
