# 快速入门

## 🛠️ 环境准备<a name="prepare&install"></a>

根据**本地是否有NPU设备**和**使用目标**选择对应的环境准备方式：

| 环境情况 | 用于社区体验 / 算子开发（CANN商用/社区版） | 用于生态开发（CANN master） |
| :---: | :---: | :---: |
| **无NPU设备** | [云开发环境](#1️⃣-云开发环境) | [云开发环境](#1️⃣-云开发环境) + [手动下载安装CANN master](#cann-install)|
| **有NPU设备** | [基于CANN镜像的Docker](#2️⃣-基于cann镜像的docker) | [Dev Container](#3️⃣-devcontainer) + [手动下载安装CANN master](#cann-install) |

> [!TIP] 选择建议
>
> - 为了保障开发体验环境的质量，推荐用户基于**容器化技术**完成**环境准备**。
> - 如不希望使用容器，也可在带NPU设备的主机上完成**环境准备**，请参考[CANN软件安装指南 - 在物理机上安装](https://www.hiascend.com/document/redirect/CannCommunityInstWizard)。
> - 针对仅体验"编译安装本开源仓 + 仿真环境运行算子"的用户，不要求主机带NPU设备，可跳过安装NPU驱动和固件，直接安装CANN包，请参考[下载安装CANN包](#cann-install)。

### 1️⃣ 云开发环境<a name="cloud-dev-env"></a>

对于无NPU设备的用户，可直接使用**CANNLab云开发环境**，即"**一站式开发平台**"，该平台提供了在线可直接运行的昇腾ARM架构环境，环境中已安装必备的驱动固件、软件包和依赖，无需手动安装。该平台目前仅适用于Atlas A2系列产品，提供两种接入方式：

- **WebIDE**，提供网页版的便携开发体验。
- **VSCode IDE**，支持远程连接**云开发环境**，提供VSCode强大插件市场的支持。

1. 进入开源仓Gitcode页面，单击"`CANNLab > 云开发`"按钮，使用已认证过的华为云账号登录。若未注册或认证，请根据页面提示进行注册和认证。

   <p align="center"><img src="figures/cloudIDE.png" alt="云平台" width="750px" height="90px"></p>

2. 根据页面提示创建NPU环境并配置规格，启动云开发环境后，单击"`连接 > WebIDE 或 Visual Studio Code`"进入一站式开发平台。

   当前开源项目资源默认在/mnt/workspace/gitCode/${gitCode_id}目录下，${gitCode_id}表示开发者个人gitCode账号。

   <p align="center"><img src="figures/webIDE.png" alt="云平台" width="1000px" height="150px"></p>

> [!NOTE] 使用说明
>
> - 环境默认安装了最新的商用版NPU驱动和固件、CANN包，源码下载时注意与软件配套。
> - 如需下载特定版本的CANN包，请参考[下载安装CANN包](#cann-install)。
> - 更多关于**CANNLab云开发环境**的介绍，请参考[CANNLab指导](https://gitcode.com/org/cann/discussions/54)。
> - [Huawei Developer Space插件](https://marketplace.visualstudio.com/items?itemName=HuaweiCloud.developerspace)为VSCode IDE接入**云开发环境**提供技术支持。

### 2️⃣ 基于CANN镜像的Docker

对于有NPU设备的用户，可使用此环境进行开发体验。

1. 确认主机环境

   - 是否已安装NPU驱动和固件，使用`npu-smi info`能够输出NPU相关信息，如没有安装，请参考《[CANN软件安装指南](https://www.hiascend.com/document/redirect/CannCommunityInstWizard)》中"准备软件包"和"安装NPU驱动和固件"章节。驱动与固件是运行态依赖，若仅编译本项目源码，可以不安装。
   - 是否已安装Docker，使用`docker --version`能够输出Docker版本信息，如没有安装，请参考[Docker官方安装指南](https://docs.docker.com/engine/install/)。

2. 下载CANN镜像

    通过以下命令从[昇腾镜像仓库](https://www.hiascend.com/developer/ascendhub/detail/17da20d1c2b6493cb38765adeba85884)拉取已预集成CANN镜像：

    ```bash
    # 命令格式为docker pull <ascend/cann:tag>
    # 示例：拉取ascend/cann:tag为9.0.0-beta.2的CANN社区包
    docker pull swr.cn-south-1.myhuaweicloud.com/ascendhub/cann:9.0.0-beta.2-910b-ubuntu22.04-py3.11
    ```

    > [!NOTE] 使用说明
    > - 镜像默认安装了对应版本的CANN包，源码下载时注意与软件配套。
    > - 镜像文件比较大，正常网速下，下载时间约为5～10分钟，请您耐心等待。

3. 运行Docker

    拉取镜像后，需要以特定参数启动，以便容器内能访问宿主机的NPU设备。

    ```bash
    docker run --name <cann_container> \
        --ipc=host --net=host --privileged \
        --device /dev/davinci0 \
        --device /dev/davinci_manager \
        --device /dev/devmm_svm \
        --device /dev/hisi_hdc \
        -v /usr/local/dcmi:/usr/local/dcmi \
        -v /usr/local/bin/npu-smi:/usr/local/bin/npu-smi \
        -v /usr/local/Ascend/driver/lib64/:/usr/local/Ascend/driver/lib64/ \
        -v /usr/local/Ascend/driver/version.info:/usr/local/Ascend/driver/version.info \
        -v /etc/ascend_install.info:/etc/ascend_install.info \
        -v </home/your_host_dir>:</home/your_container_dir> \
        -it <ascend/cann:tag> bash
    ```

    | 参数 | 说明 | 注意事项 |
    | :--- | :--- | :--- |
    | `--name <cann_container>` | 为容器指定名称，便于管理 | 自定义 |
    | `--ipc=host` | 与宿主机共享IPC命名空间，NPU进程间通信（共享内存、信号量）所需 | - |
    | `--net=host` | 使用宿主机网络栈，避免容器网络转发带来的通信延迟 | - |
    | `--privileged` | 赋予容器完整设备访问权限，NPU驱动正常工作所需 | - |
    | `--device /dev/davinci0` | 将宿主机的NPU设备卡映射到容器内，可指定映射多张NPU设备卡 | 必须根据实际情况调整：`davinci0`对应系统中的第0张NPU卡。请先在宿主机执行`npu-smi info`命令，根据输出显示的设备号（如`NPU 0`, `NPU 1`）来修改此编号 |
    | `--device /dev/davinci_manager` | 映射NPU设备管理接口 | - |
    | `--device /dev/devmm_svm` | 映射设备内存管理接口 | - |
    | `--device /dev/hisi_hdc` | 映射主机与设备间的通信接口 | - |
    | `-v /usr/local/dcmi:/usr/local/dcmi` | 挂载设备容器管理接口（DCMI）相关工具和库 | - |
    | `-v /usr/local/bin/npu-smi:/usr/local/bin/npu-smi` | 挂载`npu-smi`工具 | 使容器内可以直接运行此命令来查询NPU状态和性能信息 |
    | `-v /usr/local/Ascend/driver/lib64/:/usr/local/Ascend/driver/lib64/` | 将宿主机的NPU驱动库映射到容器内 | - |
    | `-v /usr/local/Ascend/driver/version.info:/usr/local/Ascend/driver/version.info` | 挂载驱动版本信息文件 | - |
    | `-v /etc/ascend_install.info:/etc/ascend_install.info` | 挂载CANN软件安装信息文件 | - |
    | `-v </home/your_host_dir>:</home/your_container_dir>` | 挂载宿主机的一个路径到容器中 | 自定义 |
    | `-it` | `-i`（交互式）和`-t`（分配伪终端）的组合参数 | - |
    | `<ascend/cann:tag>` | 指定要运行的Docker镜像 | 请确保此镜像名和标签（tag）与您通过`docker pull`拉取的镜像完全一致 |
    | `bash` | 容器启动后立即执行的命令 | - |

### 3️⃣ DevContainer

对于有NPU设备的用户，推荐使用此环境进行生态开发者贡献。

DevContainer基于VS Code Dev Containers，通过仓库内`.devcontainer`配置自动构建一致的容器化开发环境，内置`conda`、`Python`等开发工具链。与宿主机的NPU驱动共享设备访问，适合需要编译源码、运行UT、向本仓贡献代码的场景。详细说明请参考[`.devcontainer/README.md`](../../.devcontainer/README.md)。

> [!NOTE] 使用说明
> DevContainer仅挂载宿主机的NPU驱动（只读），**CANN toolkit和ops包需在容器启动后手动安装**，请参考[下载安装CANN包](#cann-install)。

### 📥 下载安装CANN包<a name="cann-install"></a>
<!-- 待加入AI Agent下载 & 安装CANN包的skill -->
CANN包分为CANN toolkit包和CANN ops包。

#### 下载CANN包

1. <a name="下载-cann-商用社区版"></a>下载CANN商用/社区版

    如果您想体验**官网正式发布的CANN包**，请访问[CANN安装部署-昇腾社区](https://www.hiascend.com/cann/download)获取对应版本CANN包。

2. <a name="下载-cann-master"></a>下载CANN master

    如果您想体验**CANN master**，请访问[CANN master obs镜像网站](https://ascend.devcloud.huaweicloud.com/artifactory/cann-run-mirror/software/master)，下载**日期最新**的CANN包。

#### 安装CANN包

1. 安装CANN toolkit包 (必选)

    ```bash
    chmod +x Ascend-cann-toolkit_${cann_version}_linux-$(uname -m).run
    ./Ascend-cann-toolkit_${cann_version}_linux-$(uname -m).run --install --install-path=${install_path}
    ```

2. 安装CANN ops包 (可选)

    ```bash
    chmod +x Ascend-cann-${soc_name}-ops_${cann_version}_linux-$(uname -m).run
    ./Ascend-cann-${soc_name}-ops_${cann_version}_linux-$(uname -m).run --install --install-path=${install_path}
    ```

    > [!IMPORTANT] 安装说明
    > [examples](../../examples/)中部分算子样例的编译运行依赖本包，若想完整体验样例编译运行流程，建议安装此包。

| 参数 | 说明 |
| :--- | :--- |
| `${cann_version}` | CANN包版本号 |
| `${soc_name}` | NPU型号，如`910b` |
| `${install_path}` | 安装路径，toolkit包和ops包需相同。默认：root用户`/usr/local/Ascend`，非root用户`$HOME/Ascend` |

## ✅ 环境验证<a name="cann-verify"></a>

> [!NOTE] 使用前须知
> 云开发环境和基于CANN镜像的Docker已预装CANN包，可直接执行以下命令验证；DevContainer和手动安装用户请在安装CANN包后执行。

验证环境和驱动是否正常：

- **检查NPU设备**：

    ```bash
    # 运行npu-smi，若能正常显示设备信息，则驱动正常
    npu-smi info
    ```

- **检查CANN包安装**:

    ```bash
    # 查看CANN Toolkit的version字段提供的版本信息（默认路径安装）。CANNLab场景下，请将/usr/local替换为/home/developer
    cat /usr/local/Ascend/cann/$(uname -m)-linux/ascend_toolkit_install.info
    # 查看CANN ops的version字段提供的版本信息（默认路径安装）。CANNLab场景下，请将/usr/local替换为/home/developer
    cat /usr/local/Ascend/cann/$(uname -m)-linux/ascend_ops_install.info
    ```

## ⚙️ 环境变量配置<a name="cann-env-setup"></a>

> [!NOTE] 使用前须知
> 云开发环境和基于CANN镜像的Docker已自动配置环境变量，可跳过此步骤。

按需选择合适的命令使环境变量生效：

```bash
# 默认路径安装，以root用户为例（非root用户，将/usr/local替换为${HOME}）
source /usr/local/Ascend/cann/set_env.sh
# 指定路径安装
# source ${install_path}/cann/set_env.sh
```

## 🔨 源码编译步骤

### 📥 下载源码

    可以使用以下两种方式下载，请选择其中一种进行源码准备。

- 若您的编译环境可以访问网络，建议通过以下命令行方式下载（下载时间较长，但步骤简单）。

  ```bash
  # 开发环境，非root用户命令行中执行以下命令下载源码仓。git_clone_path为用户自己创建的某个目录。
  cd ${git_clone_path}
  git clone https://gitcode.com/cann/asc-tools.git
  ```
  **注：如果需要切换到其它tag版本，以v0.5.0为例，可执行以下命令。**
  ```bash
  git checkout v0.5.0
  ```
- 若您的编译环境无法访问网络，可以通过压缩包方式下载（下载时间较短，但步骤稍微复杂）。

  **注：如果需要下载其它版本代码，请先根据前置条件说明进行asc-tools仓分支切换。下载压缩包命名跟tag/branch相关，此处以master分支为例，下载的名字将会是asc-tools-master.zip**
  ```bash
  # 1. asc-tools仓右上角选择 【下载ZIP】。
  # 2. 将ZIP包上传到开发环境中的普通用户某个目录中，【例如：${git_clone_path}/asc-tools-master.zip】。
  # 3. 开发环境中，执行以下命令，解压zip包。
  cd ${git_clone_path}
  unzip asc-tools-master.zip
  ```
  并且，需要同样操作下载依赖的[msot](https://gitcode.com/Ascend/msot.git)、[mssanitizer](https://gitcode.com/Ascend/mssanitizer.git)、[msopprof](https://gitcode.com/Ascend/msopprof.git)、[msopgen](https://gitcode.com/Ascend/msopgen.git)、[mskpp](https://gitcode.com/Ascend/mskpp.git)、[mskl](https://gitcode.com/Ascend/mskl.git)、[msdebug](https://gitcode.com/Ascend/msdebug.git)代码仓的master分支的压缩包，即点击各仓链接，右上角选择【下载ZIP】。

  同时，需要根据实际环境，下载对应Release版本的闭源cpudebug包（[cpudebug x86_64包](https://container-obsfs-filesystem.obs.cn-north-4.myhuaweicloud.com/package/cann/asc-toolkit-dev/version_compile/master/202601/20260131_174635_/ubuntu_x86/cann-asc-tools-cpudebug-deps-lib_release_9.0.0_linux-x86_64.tar.gz)、[cpudebug aarch64包](https://container-obsfs-filesystem.obs.cn-north-4.myhuaweicloud.com/package/cann/asc-toolkit-dev/version_compile/master/202601/20260131_174635_/ubuntu_aarch64/cann-asc-tools-cpudebug-deps-lib_release_9.0.0_linux-aarch64.tar.gz)），以及开源第三方软件依赖，列表如下：

  | 开源软件 | 版本 | 下载地址 |
  |---|---|---|
  | makeself | 2.5.0 | [makeself-release-2.5.0.tar.gz](https://cann-3rd.obs.cn-north-4.myhuaweicloud.com/makeself/makeself-release-2.5.0.tar.gz) |
  | makeself_patch | 2.5.0 | [patch/makeself-2.5.0.patch](https://cann-3rd.obs.cn-north-4.myhuaweicloud.com/makeself/fix/makeself-2.5.0.patch) |
  | boost | 1.87.0 | [boost-1_87_0.tar.gz](https://gitcode.com/cann-src-third-party/boost/releases/download/v1.87.0/boost_1_87_0.tar.gz) |
  | googletest | 1.14.0 | [googletest-1.14.0.tar.gz](https://gitcode.com/cann-src-third-party/googletest/releases/download/v1.14.0/googletest-1.14.0.tar.gz) |
  | mockcpp | 2.7 | [mockcpp-2.7.tar.gz](https://gitcode.com/cann-src-third-party/mockcpp/releases/download/v2.7-h2/mockcpp-2.7.tar.gz) |
  | mockcpp_patch | 2.7 | [mockcpp-2.7.patch](https://gitcode.com/cann-src-third-party/mockcpp/releases/download/v2.7-h3/mockcpp-2.7_py3-h3.patch) |
  | cann-cmake | master-059 | [cmake-master-059.tar.gz](https://raw.gitcode.com/cann/cmake/archive/refs/heads/master-059.tar.gz) |

  其中，对于依赖的开源第三方软件，提供一键式下载脚本`install_dep_tar.sh`，用户可以通过执行如下命令，下载所有开源第三方软件依赖。
  ```bash
  bash install_dep_tar.sh --dest_dir=${your_3rd_party_path} # 其中，${your_3rd_party_path}为下载开源第三方软件的存放路径
  ```

  默认下载到当前目录的 `third_party`，补丁保存在下载目录的 `patch` 子目录。下载文件均进行 SHA256 校验，有效缓存会跳过，下载或校验失败时返回非零退出码。编译时请将 `--cann_3rd_lib_path` 设置为同一目录。

## 安装依赖

> [!NOTE] 使用前须知
> 如果您使用**容器化技术**，容器中已为您安装好依赖，可跳过此步骤。

   以下所列仅为本开源仓源码编译用到的依赖，可参考仓库内[DevContainer Python依赖（以 Python 3.12 为例）](../../.devcontainer/requirements.txt)，其中python、gcc、cmake的安装方法请参见配套版本的[用户手册](https://hiascend.com/document/redirect/CannCommunityInstDepend)，选择安装场景后，参见"安装CANN > 安装依赖"章节进行相关依赖的安装。

   - python >= 3.7.0 (注意：python宣布3.7.x/3.8.x版本已经EOL，CANN即将停止对该版本的支持，请升级到>=3.9.x版本)

   - gcc >= 7.3.0 / g++ >= 7.3.0 (注意：要求gcc与g++版本一致)

   - cmake >= 3.16.0

   - ccache >= 4.6.1

     建议版本[release-v4.6.1](https://github.com/ccache/ccache/releases/tag/v4.6.1)，x86_64环境[下载链接](https://github.com/ccache/ccache/releases/download/v4.6.1/ccache-4.6.1-linux-x86_64.tar.xz)，aarch64环境[下载链接](https://github.com/ccache/ccache/releases/download/v4.6.1/ccache-4.6.1.tar.gz)。

     x86_64环境安装步骤如下：

     ```bash
     # 在准备安装的路径下创建buildtools目录，如有则忽略
     # 这里以安装路径/opt为例，对安装命令进行说明
     mkdir /opt/buildtools
     # 切换到安装包下载路径，将ccache解压到安装路径
     tar -xf ccache-4.6.1-linux-x86_64.tar.xz -C /opt/buildtools
     chmod 755 /opt/buildtools/ccache-4.6.1-linux-x86_64/ccache
     mkdir -p /usr/local/ccache/bin
     # 建立软链接
     ln -sf /opt/buildtools/ccache-4.6.1-linux-x86_64/ccache /usr/local/bin/ccache
     ln -sf /opt/buildtools/ccache-4.6.1-linux-x86_64/ccache /usr/local/ccache/bin/ccache
     # 将ccache添加到环境变量PATH
     export PATH=/usr/local/ccache/bin:$PATH
     ```

     aarch64环境安装步骤如下：
     - 下载依赖项
       下载[zstd](https://github.com/facebook/zstd/releases/download/v1.5.0/zstd-1.5.0.tar.gz)和[hiredis](https://github.com/redis/hiredis/archive/refs/tags/v1.0.2.tar.gz)。

     - 编译安装

        ```bash
        # 在准备安装的路径下创建buildtools目录，如有则忽略
        # 这里以安装路径/opt为例，对安装命令进行说明
        mkdir /opt/buildtools
        # 切换到安装包下载路径，将zstd解压到安装路径
        tar -xf zstd-1.5.0.tar.gz -C /opt/buildtools
        cd /opt/buildtools/zstd-1.5.0
        make -j 24
        make install
        cd -
        # 切换到安装包下载路径，将hiredis解压到安装路径
        tar -xf hiredis-1.0.2.tar.gz -C /opt/buildtools
        cd /opt/buildtools/hiredis-1.0.2
        make -j 24 prefix=/opt/buildtools/hiredis-1.0.2 all
        make prefix=/opt/buildtools/hiredis-1.0.2 install
        cd -
        # 切换到安装包下载路径，将ccache解压到安装路径
        tar -xf ccache-4.6.1.tar.gz -C /opt/buildtools
        cd /opt/buildtools/ccache-4.6.1
        mkdir build
        cd build/
        cmake -DCMAKE_BUILD_TYPE=Release -DZSTD_LIBRARY=/usr/local/lib/libzstd.a -DZSTD_INCLUDE_DIR=/usr/local/include -DHIREDIS_LIBRARY=/usr/local/lib/libhiredis.a -DHIREDIS_INCLUDE_DIR=/usr/local/include ..
        make -j 24
        make install
        mkdir -p /usr/local/ccache/bin
        # 建立软链接
        ln -sf /usr/local/bin/ccache /usr/local/ccache/bin/ccache
        # 将ccache添加到环境变量PATH
        export PATH=/usr/local/ccache/bin:$PATH
        ```

   - setuptools >= 45.2.0

     执行以下命令安装：
     ```bash
     pip3 install setuptools
     ```

   - lcov >= 1.13（可选，仅执行UT时依赖）

     以Ubuntu系统为例，x86_64环境执行以下命令安装：
     ```bash
     apt install lcov
     ```
     以Euler系统为例，aarch64环境执行以下命令安装：
     ```bash
     yum install lcov
     ```

   - pytest >= 8.3.2（可选，仅执行UT时依赖）

     执行以下命令安装：
     ```bash
     pip3 install pytest
     ```

   - coverage >= 4.5.4（可选，仅执行UT时依赖）

     执行以下命令安装：
     ```bash
     pip3 install coverage
     ```
   - googletest（可选，仅执行UT时依赖，建议版本[release-1.11.0](https://github.com/google/googletest/releases/tag/release-1.11.0)）

     下载[googletest源码](https://github.com/google/googletest.git)后，执行以下命令安装：

     ```bash
     mkdir temp && cd temp                 # 在googletest源码根目录下创建临时目录并进入
     cmake .. -DCMAKE_CXX_FLAGS="-fPIC -D_GLIBCXX_USE_CXX11_ABI=0"
     make
     make install                         # root用户安装googletest
     # sudo make install                  # 非root用户安装googletest
     ```

## 编译安装<a name="compile&install"></a>

1. 编译

  本开源仓提供一键式编译安装能力，进入本开源仓代码根目录，执行如下命令：

  ```bash
  cd asc-tools
  bash build.sh --pkg
  ```
  若您的编译环境无法访问网络，您需要在联网环境中下载上述依赖代码仓的压缩包、闭源压缩包及开源软件压缩包，并手动上传至您的编译环境中。

  您需要在编译环境中新建一个`{your_3rd_party_path}`目录来存放依赖代码仓、闭源及第三方开源软件的压缩包。

  ```bash
  mkdir -p {your_3rd_party_path}
  ```

  创建好目录后，将下载好的所依赖的压缩包上传至目录`{your_3rd_party_path}`后，可以使用如下命令进行编译：
  ```bash
  bash build.sh --pkg --cann_3rd_lib_path={your_3rd_party_path}
  ```

  编译完成后会在`build_out`目录下生成`cann-asc-tools_*<cann_version>*_linux-*<arch>*.run`软件包。

   > [!CAUTION] 编译报错可能
   > 本仓依赖其他CANN开源仓，**暂不支持独立升级**，须搭配对应版本的CANN包进行编译：
   > - master分支 -- 使用**最新的**[CANN master包](#下载-cann-master)
   > - 特定Tag -- 使用对应版本的[官网正式发布的CANN包](#下载-cann-商用社区版)

2. 安装

  在开源仓根目录下执行下列命令，根据设置的环境变量路径，将编译生成的run包安装到CANN包的装包路径，同时会覆盖原CANN包中的Ascend C内容。

  ```bash
  # 切换到run包生成路径下
  cd build_out
  # 默认路径安装run包
  ./cann-asc-tools_<cann_version>_linux-<arch>.run --full --pylocal
  # 指定路径安装run包
  ./cann-asc-tools_<cann_version>_linux-<arch>.run --full --pylocal --install-path=${install_path}
  ```

## UT测试（可选）

在开源仓根目录执行下列命令之一，将依次批跑tests目录下的用例，得到结果日志，用于看护编译是否正常。

```bash
bash build.sh -t
```

或

```bash
bash build.sh --test
```

若您的编译环境无法访问网络，您需要在联网环境中下载上述依赖代码仓的压缩包及开源软件压缩包，并手动上传至您的环境中。
  同时，相比于编译，UT不下载Release版本的cpudebug包，而是需要根据实际环境，下载对应Debug版本的闭源cpudebug包（[cpudebug x86_64包](https://container-obsfs-filesystem.obs.cn-north-4.myhuaweicloud.com/package/cann/asc-toolkit-dev/version_compile/master/202601/20260131_174635_/ubuntu_x86/cann-asc-tools-cpudebug-deps-lib_debug_9.0.0_linux-x86_64.tar.gz)、[cpudebug aarch64包](https://container-obsfs-filesystem.obs.cn-north-4.myhuaweicloud.com/package/cann/asc-toolkit-dev/version_compile/master/202601/20260131_174635_/ubuntu_aarch64/cann-asc-tools-cpudebug-deps-lib_debug_9.0.0_linux-aarch64.tar.gz)），Debug版本相比Release版本，更方便进行调试。

  您需要在环境中新建一个`{your_3rd_party_path}`目录来存放依赖代码仓、闭源及第三方开源软件的压缩包。

  ```bash
  mkdir -p {your_3rd_party_path}
  ```

  创建好目录后，将下载好的所依赖的压缩包上传至目录`{your_3rd_party_path}`后，可以使用如下命令进行UT测试：
  ```bash
  bash build.sh --test --cann_3rd_lib_path={your_3rd_party_path}
  ```

### UT测试显示覆盖率

- 依赖项
    - lcov >= 1.14

- 执行命令

```bash
bash build.sh --test --cov
```
