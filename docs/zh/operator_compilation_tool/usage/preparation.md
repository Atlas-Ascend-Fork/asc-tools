# 使用前准备

## 环境准备

1. 安装软件包。请参考《[CANN软件安装](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/latest/softwareinst/instg/instg_0000.html?OS=openEuler&InstallType=netyum)》完成驱动、固件、CANN软件包的安装。
2. 配置环境变量。

    CANN提供进程级环境变量设置脚本，供用户在进程中引用，以自动完成环境变量设置。执行命令参考如下，以下示例均为root或非root用户默认安装路径，请以实际安装路径为准。

    ```bash
    # 以root用户安装toolkit包后配置环境变量
    source /usr/local/Ascend/cann/set_env.sh
    # 以非root用户安装toolkit包后配置环境变量
    source ${HOME}/Ascend/cann/set_env.sh
    ```

3. 调试版本编译场景下，需要开发者安装问题发生时实际使用的二进制包，包括静态二进制包和动态二进制包。

## 工具获取

工具所在目录：`${INSTALL_DIR}/bin/op_compiler`。

`${INSTALL_DIR}`请替换为CANN软件安装后文件存储路径。以root用户安装为例，安装后文件默认存储路径为：`/usr/local/Ascend/cann`。
