# Quick Start

## 🛠️ Environment Preparation<a name="prepare&install"></a>

Choose the appropriate environment preparation method based on **whether you have an NPU device locally** and **your usage goal**:

| Environment | For Community Experience / Operator Development (CANN Commercial/Community Edition) | For Ecosystem Development (CANN master) |
| :---: | :---: | :---: |
| **No NPU device** | [Cloud Development Environment](#1️⃣-cloud-development-environment) | [Cloud Development Environment](#1️⃣-cloud-development-environment) + [Manually Download and Install CANN master](#📥 Download and Install CANN Packages)|
| **With NPU device** | [Docker Based on CANN Image](#2️⃣-docker-based-on-cann-image) | [Dev Container](#3️⃣-devcontainer) + [Manually Download and Install CANN master](#📥 Download and Install CANN Packages) |

> [!TIP] Selection Guide
>
> - To ensure a quality development experience, it is recommended to use **containerization technology** for **environment preparation**.
> - If you prefer not to use containers, you can also set up the environment on a host with an NPU device. Please refer to [CANN Software Installation Guide - Installing on a Physical Machine](https://www.hiascend.com/document/redirect/CannCommunityInstWizard).
> - For users who only want to experience "compiling and installing this open-source repository + running operators in a simulation environment", an NPU device is not required. You can skip installing the NPU driver and firmware, and directly install the CANN package. Please refer to [Download and Install CANN Packages](#📥 Download and Install CANN Packages).

### 1️⃣ Cloud Development Environment<a name="cloud-dev-env"></a>

For users without NPU devices, you can directly use the **CANNLab Cloud Development Environment**, also known as the "**One-Stop Development Platform**". This platform provides an online Ascend ARM architecture environment that is ready to run, with all required drivers, firmware, software packages, and dependencies pre-installed — no manual installation needed. This platform currently only supports Atlas A2 series products and offers two access methods:

- **WebIDE**: Provides a portable web-based development experience.
- **VSCode IDE**: Supports remote connection to the **Cloud Development Environment** with access to the powerful VSCode plugin marketplace.

1. Go to the GitCode page of the open-source repository, click the "`CANNLab > Cloud Development`" button, and log in with your verified Huawei Cloud account. If you have not registered or verified your account, please follow the on-screen instructions to complete registration and verification.

   <p align="center"><img src="../zh/figures/cloudIDE.png" alt="Cloud Platform" width="750px" height="90px"></p>

2. Follow the on-screen instructions to create an NPU environment, configure specifications, and start the cloud development environment. Then click "`Connect > WebIDE or Visual Studio Code`" to enter the one-stop development platform.

   The current open-source project resources are located in the /mnt/workspace/gitCode/\${gitCode_id} directory by default, where \${gitCode_id} represents the developer's personal GitCode account.

   <p align="center"><img src="../zh/figures/webIDE.png" alt="Cloud Platform" width="1000px" height="150px"></p>

> [!NOTE] Usage Notes
>
> - The environment comes pre-installed with the latest commercial NPU driver, firmware, and CANN package. Make sure the downloaded source code is compatible with the installed software version.
> - To download a specific version of the CANN package, please refer to [Download and Install CANN Packages](#📥 Download and Install CANN Packages).
> - For more information about the **CANNLab Cloud Development Environment**, please refer to [CANNLab Guide](https://gitcode.com/org/cann/discussions/54).
> - The [Huawei Developer Space Plugin](https://marketplace.visualstudio.com/items?itemName=HuaweiCloud.developerspace) provides technical support for connecting VSCode IDE to the **Cloud Development Environment**.

### 2️⃣ Docker Based on CANN Image

For users with NPU devices, this environment can be used for development and experience.

1. Verify the host environment

   - Check whether the NPU driver and firmware are installed. Run `npu-smi info` to display NPU information. If not installed, refer to the "Prepare Software Packages" and "Install NPU Driver and Firmware" sections in the [CANN Software Installation Guide](https://www.hiascend.com/document/redirect/CannCommunityInstWizard). The driver and firmware are runtime dependencies and are not required if you only need to compile the source code of this project.
   - Check whether Docker is installed. Run `docker --version` to display Docker version information. If not installed, refer to the [Docker Official Installation Guide](https://docs.docker.com/engine/install/).

2. Pull the CANN image

    Pull the pre-integrated CANN image from the [Ascend Image Repository](https://www.hiascend.com/developer/ascendhub/detail/17da20d1c2b6493cb38765adeba85884) using the following command:

    ```bash
    # Command format: docker pull <ascend/cann:tag>
    # Example: Pull the CANN community package with ascend/cann:tag 9.0.0-beta.2
    docker pull swr.cn-south-1.myhuaweicloud.com/ascendhub/cann:9.0.0-beta.2-910b-ubuntu22.04-py3.11
    ```

    > [!NOTE] Usage Notes
    > - The image comes pre-installed with the corresponding version of the CANN package. Make sure the downloaded source code is compatible with the installed software version.
    > - The image file is relatively large. Under normal network conditions, the download takes approximately 5–10 minutes. Please be patient.

3. Run Docker

    After pulling the image, you need to start the container with specific parameters so that the NPU device on the host can be accessed inside the container.

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

    | Parameter | Description | Notes |
    | --- | --- | --- |
    | `--name <cann_container>` | Specify a name for the container for easy management | Customizable |
    | `--ipc=host` | Share the IPC namespace with the host, required for inter-process communication (shared memory, semaphores) of NPU processes | - |
    | `--net=host` | Use the host's network stack to avoid communication latency caused by container network forwarding | - |
    | `--privileged` | Grant the container full device access permissions, required for the NPU driver to function properly | - |
    | `--device /dev/davinci0` | Map the host's NPU device card into the container. Multiple NPU cards can be specified | Must be adjusted based on the actual setup: `davinci0` corresponds to the first NPU card (index 0) in the system. Run `npu-smi info` on the host first, and modify this number based on the device IDs shown in the output (e.g., `NPU 0`, `NPU 1`) |
    | `--device /dev/davinci_manager` | Map the NPU device management interface | - |
    | `--device /dev/devmm_svm` | Map the device memory management interface | - |
    | `--device /dev/hisi_hdc` | Map the communication interface between the host and the device | - |
    | `-v /usr/local/dcmi:/usr/local/dcmi` | Mount the Device Container Management Interface (DCMI) related tools and libraries | - |
    | `-v /usr/local/bin/npu-smi:/usr/local/bin/npu-smi` | Mount the `npu-smi` tool | Allows running this command inside the container to query NPU status and performance information |
    | `-v /usr/local/Ascend/driver/lib64/:/usr/local/Ascend/driver/lib64/` | Map the host's NPU driver libraries into the container | - |
    | `-v /usr/local/Ascend/driver/version.info:/usr/local/Ascend/driver/version.info` | Mount the driver version information file | - |
    | `-v /etc/ascend_install.info:/etc/ascend_install.info` | Mount the CANN software installation information file | - |
    | `-v </home/your_host_dir>:</home/your_container_dir>` | Mount a host path into the container | Customizable |
    | `-it` | Combination of `-i` (interactive) and `-t` (allocate a pseudo-terminal) | - |
    | `<ascend/cann:tag>` | Specify the Docker image to run | Ensure this image name and tag exactly match the image you pulled via `docker pull` |
    | `bash` | Command to execute immediately after the container starts | - |

### 3️⃣ DevContainer

For users with NPU devices, this environment is recommended for ecosystem development contributions.

DevContainer is based on VS Code Dev Containers and automatically builds a consistent containerized development environment through the `.devcontainer` configuration in the repository, with built-in toolchains such as `conda` and `Python`. It shares device access with the host's NPU driver, making it suitable for scenarios that require source code compilation, running UTs, and contributing code to this repository. For detailed information, please refer to [`.devcontainer/README.md`](../../.devcontainer/README.md).

> [!NOTE] Usage Notes
> DevContainer only mounts the host's NPU driver (read-only). **The CANN toolkit and ops packages need to be manually installed after the container starts**. Please refer to [Download and Install CANN Packages](#📥 Download and Install CANN Packages).

### 📥 Download and Install CANN Packages<a name="cann-install"></a>
<!-- TODO: Add AI Agent skill for downloading & installing CANN packages -->
CANN packages are divided into the CANN toolkit package and the CANN ops package.

#### Download CANN Packages

1. <a name="download-cann-commercialcommunity-edition"></a>Download CANN Commercial/Community Edition

    If you want to experience the **officially released CANN packages** from the website, please visit [CANN Installation and Deployment - Ascend Community](https://www.hiascend.com/cann/download) to obtain the corresponding version of the CANN package.

2. <a name="download-cann-master"></a>Download CANN master

    If you want to experience **CANN master**, please visit the [CANN master OBS mirror site](https://ascend.devcloud.huaweicloud.com/artifactory/cann-run-mirror/software/master) and download the CANN package with the **latest date**.

#### Install CANN Packages

1. Install the CANN toolkit package (Required)

    ```bash
    chmod +x Ascend-cann-toolkit_${cann_version}_linux-$(uname -m).run
    ./Ascend-cann-toolkit_${cann_version}_linux-$(uname -m).run --install --install-path=${install_path}
    ```

2. Install the CANN ops package (Optional)

    ```bash
    chmod +x Ascend-cann-${soc_name}-ops_${cann_version}_linux-$(uname -m).run
    ./Ascend-cann-${soc_name}-ops_${cann_version}_linux-$(uname -m).run --install --install-path=${install_path}
    ```

    > [!IMPORTANT] Installation Notes
    > Some operator examples in [examples](../../examples/) require this package for compilation and execution. To fully experience the example compilation and execution workflow, it is recommended to install this package.

| Parameter | Description |
| :--- | :--- |
| `${cann_version}` | CANN package version number |
| `${soc_name}` | NPU model, e.g., `910b` |
| `${install_path}` | Installation path. The toolkit and ops packages must use the same path. Default: `/usr/local/Ascend` for root user, `$HOME/Ascend` for non-root user |

## ✅ Environment Verification<a name="cann-verify"></a>

> [!NOTE] Before You Start
> The Cloud Development Environment and Docker based on CANN image come with CANN packages pre-installed. You can directly run the following commands for verification. DevContainer and manual installation users should run these commands after installing the CANN packages.

Verify that the environment and drivers are functioning properly:

- **Check NPU device**:

    ```bash
    # Run npu-smi. If device information is displayed correctly, the driver is working properly
    npu-smi info
    ```

- **Check CANN package installation**:

    ```bash
    # View the version field of CANN Toolkit (default installation path). For CANNLab scenarios, replace /usr/local with /home/developer
    cat /usr/local/Ascend/cann/$(uname -m)-linux/ascend_toolkit_install.info
    # View the version field of CANN ops (default installation path). For CANNLab scenarios, replace /usr/local with /home/developer
    cat /usr/local/Ascend/cann/$(uname -m)-linux/ascend_ops_install.info
    ```

## ⚙️ Environment Variable Configuration<a name="cann-env-setup"></a>

> [!NOTE] Before You Start
> The Cloud Development Environment and Docker based on CANN image have environment variables automatically configured. You can skip this step.

Choose the appropriate command to apply the environment variables as needed:

```bash
# Default installation path, using root user as an example (for non-root users, replace /usr/local with ${HOME})
source /usr/local/Ascend/cann/set_env.sh
# Custom installation path
# source ${install_path}/cann/set_env.sh
```

## 🔨 Source Code Compilation Steps

### 📥 Download Source Code

    You can use one of the following two methods to download the source code.

- If your build environment has network access, it is recommended to use the following command-line method (longer download time, but simpler steps).

  ```bash
  # Development environment. Run the following command as a non-root user to download the source repository. git_clone_path is a directory created by the user.
  cd ${git_clone_path}
  git clone https://gitcode.com/cann/asc-tools.git
  ```

  **Note: To switch to another tag version, e.g., v0.5.0, run the following command.**

  ```bash
  git checkout v0.5.0
  ```

- If your build environment does not have network access, you can download via a ZIP package (shorter download time, but slightly more complex steps).

  **Note: To download other versions of the code, switch the asc-tools repository branch first according to the prerequisites. The downloaded ZIP file name is related to the tag/branch. Here we use the master branch as an example, and the downloaded file will be named asc-tools-master.zip**

  ```bash
  # 1. On the asc-tools repository page, click [Download ZIP] in the upper-right corner.
  # 2. Upload the ZIP package to a directory in the development environment under a regular user, e.g., [${git_clone_path}/asc-tools-master.zip].
  # 3. In the development environment, run the following command to extract the ZIP package.
  cd ${git_clone_path}
  unzip asc-tools-master.zip
  ```

  Additionally, you need to download the master branch ZIP packages of the dependent repositories [msot](https://gitcode.com/Ascend/msot.git), [mssanitizer](https://gitcode.com/Ascend/mssanitizer.git), [msopprof](https://gitcode.com/Ascend/msopprof.git), [msopgen](https://gitcode.com/Ascend/msopgen.git), [mskpp](https://gitcode.com/Ascend/mskpp.git), [mskl](https://gitcode.com/Ascend/mskl.git), and [msdebug](https://gitcode.com/Ascend/msdebug.git) using the same method — click each repository link and select [Download ZIP] in the upper-right corner.

  Also, depending on your actual environment, download the corresponding Release version of the closed-source cpudebug package ([cpudebug x86_64 package](https://container-obsfs-filesystem.obs.cn-north-4.myhuaweicloud.com/package/cann/asc-toolkit-dev/version_compile/master/202601/20260131_174635_/ubuntu_x86/cann-asc-tools-cpudebug-deps-lib_release_9.0.0_linux-x86_64.tar.gz), [cpudebug aarch64 package](https://container-obsfs-filesystem.obs.cn-north-4.myhuaweicloud.com/package/cann/asc-toolkit-dev/version_compile/master/202601/20260131_174635_/ubuntu_aarch64/cann-asc-tools-cpudebug-deps-lib_release_9.0.0_linux-aarch64.tar.gz)), as well as the open-source third-party software dependencies listed below:

  | Open-Source Software | Version | Download Link |
  |---|---|---|
  | makeself | 2.5.0 | [makeself-release-2.5.0.tar.gz](https://cann-3rd.obs.cn-north-4.myhuaweicloud.com/makeself/makeself-release-2.5.0.tar.gz) |
  | makeself_patch | 2.5.0 | [patch/makeself-2.5.0.patch](https://cann-3rd.obs.cn-north-4.myhuaweicloud.com/makeself/fix/makeself-2.5.0.patch) |
  | boost | 1.87.0 | [boost-1_87_0.tar.gz](https://gitcode.com/cann-src-third-party/boost/releases/download/v1.87.0/boost_1_87_0.tar.gz) |
  | googletest | 1.14.0 | [googletest-1.14.0.tar.gz](https://gitcode.com/cann-src-third-party/googletest/releases/download/v1.14.0/googletest-1.14.0.tar.gz) |
  | mockcpp | 2.7 | [mockcpp-2.7.tar.gz](https://gitcode.com/cann-src-third-party/mockcpp/releases/download/v2.7-h2/mockcpp-2.7.tar.gz) |
  | mockcpp_patch | 2.7 | [mockcpp-2.7.patch](https://gitcode.com/cann-src-third-party/mockcpp/releases/download/v2.7-h3/mockcpp-2.7_py3-h3.patch) |
  | cann-cmake | master-059 | [cmake-master-059.tar.gz](https://raw.gitcode.com/cann/cmake/archive/refs/heads/master-059.tar.gz) |

  For the dependent open-source third-party software, a one-click download script `install_dep_tar.sh` is provided. Users can download all open-source third-party software dependencies by running the following command:

  ```bash
  bash install_dep_tar.sh --dest_dir=${your_3rd_party_path} # ${your_3rd_party_path} is the directory for storing the downloaded open-source third-party software
  ```

  By default, files are downloaded to `third_party` in the current directory, with patches in its `patch` subdirectory. SHA256 is verified for every file; valid cached files are skipped, and download or verification failures return a nonzero exit code. Set `--cann_3rd_lib_path` to the same directory when building.

## Install Dependencies

> [!NOTE] Before You Start
> If you are using **containerization technology**, the dependencies are already installed in the container. You can skip this step.

   The following lists only the dependencies used for compiling the source code of this open-source repository. Refer to [DevContainer Python Dependencies (using Python 3.12 as an example)](../../.devcontainer/requirements.txt) in the repository. For installation instructions for python, gcc, and cmake, please refer to the corresponding version of the [User Manual](https://hiascend.com/document/redirect/CannCommunityInstDepend), select the installation scenario, and refer to the "Install CANN > Install Dependencies" section.

   - python >= 3.7.0 (Note: Python has announced EOL for versions 3.7.x/3.8.x. CANN will soon stop supporting these versions. Please upgrade to version >= 3.9.x)

   - gcc and g++ supported version range: 7.3.x to 14.x (Note: gcc and g++ versions must match)

   - cmake >= 3.16.0

   - ccache >= 4.6.1

      Recommended version: [release-v4.6.1](https://github.com/ccache/ccache/releases/tag/v4.6.1). [Download link](https://github.com/ccache/ccache/releases/download/v4.6.1/ccache-4.6.1-linux-x86_64.tar.xz) for x86_64, [Download link](https://github.com/ccache/ccache/releases/download/v4.6.1/ccache-4.6.1.tar.gz) for aarch64.

      Installation steps for x86_64:

      ```bash
      # Create a buildtools directory under the target installation path (skip if it already exists)
      # Using /opt as the installation path for illustration
      mkdir /opt/buildtools
      # Navigate to the download path and extract ccache to the installation path
      tar -xf ccache-4.6.1-linux-x86_64.tar.xz -C /opt/buildtools
      chmod 755 /opt/buildtools/ccache-4.6.1-linux-x86_64/ccache
      mkdir -p /usr/local/ccache/bin
      # Create symbolic links
      ln -sf /opt/buildtools/ccache-4.6.1-linux-x86_64/ccache /usr/local/bin/ccache
      ln -sf /opt/buildtools/ccache-4.6.1-linux-x86_64/ccache /usr/local/ccache/bin/ccache
      # Add ccache to the PATH environment variable
      export PATH=/usr/local/ccache/bin:$PATH
      ```

      Installation steps for aarch64:
      - Download dependencies
        Download [zstd](https://github.com/facebook/zstd/releases/download/v1.5.0/zstd-1.5.0.tar.gz) and [hiredis](https://github.com/redis/hiredis/archive/refs/tags/v1.0.2.tar.gz).

      - Compile and install

         ```bash
         # Create a buildtools directory under the target installation path (skip if it already exists)
         # Using /opt as the installation path for illustration
         mkdir /opt/buildtools
         # Navigate to the download path and extract zstd to the installation path
         tar -xf zstd-1.5.0.tar.gz -C /opt/buildtools
         cd /opt/buildtools/zstd-1.5.0
         make -j 24
         make install
         cd -
         # Navigate to the download path and extract hiredis to the installation path
         tar -xf hiredis-1.0.2.tar.gz -C /opt/buildtools
         cd /opt/buildtools/hiredis-1.0.2
         make -j 24 prefix=/opt/buildtools/hiredis-1.0.2 all
         make prefix=/opt/buildtools/hiredis-1.0.2 install
         cd -
         # Navigate to the download path and extract ccache to the installation path
         tar -xf ccache-4.6.1.tar.gz -C /opt/buildtools
         cd /opt/buildtools/ccache-4.6.1
         mkdir build
         cd build/
         cmake -DCMAKE_BUILD_TYPE=Release -DZSTD_LIBRARY=/usr/local/lib/libzstd.a -DZSTD_INCLUDE_DIR=/usr/local/include -DHIREDIS_LIBRARY=/usr/local/lib/libhiredis.a -DHIREDIS_INCLUDE_DIR=/usr/local/include ..
         make -j 24
         make install
         mkdir -p /usr/local/ccache/bin
         # Create symbolic links
         ln -sf /usr/local/bin/ccache /usr/local/ccache/bin/ccache
         # Add ccache to the PATH environment variable
         export PATH=/usr/local/ccache/bin:$PATH
         ```

  - setuptools >= 45.2.0

    Run the following command to install:

    ```bash
    pip3 install setuptools
    ```

  - lcov >= 1.13 (Optional, only required for running UTs)

    For Ubuntu on x86_64, run the following command to install:

    ```bash
    apt install lcov
    ```

    For Euler on aarch64, run the following command to install:

    ```bash
    yum install lcov
    ```

  - pytest >= 8.3.2 (Optional, only required for running UTs)

    Run the following command to install:

    ```bash
    pip3 install pytest
    ```

  - coverage >= 4.5.4 (Optional, only required for running UTs)

    Run the following command to install:

    ```bash
    pip3 install coverage
    ```

  - googletest (Optional, only required for running UTs, recommended version [release-1.11.0](https://github.com/google/googletest/releases/tag/release-1.11.0))

    Download the [googletest source code](https://github.com/google/googletest.git) and run the following commands to install:

    ```bash
    mkdir temp && cd temp                 # Create a temporary directory in the googletest source root and enter it
    cmake .. -DCMAKE_CXX_FLAGS="-fPIC -D_GLIBCXX_USE_CXX11_ABI=0"
    make
    make install                         # Install googletest as root user
    # sudo make install                  # Install googletest as non-root user
    ```

## Build and Install<a name="compile&install"></a>

1. Build

   This open-source repository provides one-click build and install capabilities. Navigate to the root directory of the repository and run the following command:

   ```bash
   cd asc-tools
   bash build.sh --pkg
   ```

   If your build environment does not have network access, you need to download the dependent repository ZIP packages, closed-source ZIP packages, and open-source software ZIP packages in a network-connected environment, and manually upload them to your build environment.

   You need to create a `{your_3rd_party_path}` directory in the build environment to store the dependent repository packages, closed-source packages, and third-party open-source software packages.

   ```bash
   mkdir -p {your_3rd_party_path}
   ```

   After creating the directory, upload the downloaded dependency packages to the `{your_3rd_party_path}` directory, then use the following command to build:

   ```bash
   bash build.sh --pkg --cann_3rd_lib_path={your_3rd_party_path}
   ```

   After the build completes, the `cann-asc-tools_*<cann_version>*_linux-*<arch>*.run` package will be generated in the `build_out` directory.

    > [!CAUTION] Possible Build Errors
    > This repository depends on other CANN open-source repositories and **does not support independent upgrades yet**. It must be compiled with the matching version of the CANN package:
    > - master branch -- Use the **latest** [CANN master package](#download-cann-master)
    > - Specific Tag -- Use the corresponding version of the [officially released CANN package](#download-cann-commercialcommunity-edition)

2. Install

   Run the following commands in the root directory of the open-source repository to install the generated run package to the CANN package installation path based on the configured environment variable path. This will also overwrite the Ascend C content in the original CANN package.

   ```bash
   # Navigate to the run package output directory
   cd build_out
   # Install the run package to the default path
   ./cann-asc-tools_<cann_version>_linux-<arch>.run --full --pylocal
   # Install the run package to a custom path
   ./cann-asc-tools_<cann_version>_linux-<arch>.run --full --pylocal --install-path=${install_path}
   ```

## UT Testing (Optional)

Run one of the following commands in the root directory of the open-source repository to sequentially execute the test cases in the tests directory and generate result logs to verify the build.

```bash
bash build.sh -t
```

Or

```bash
bash build.sh --test
```

If your build environment does not have network access, you need to download the dependent repository ZIP packages and open-source software ZIP packages in a network-connected environment, and manually upload them to your environment.
  Additionally, unlike the build process, UT does not download the Release version of the cpudebug package. Instead, you need to download the corresponding Debug version of the closed-source cpudebug package based on your actual environment ([cpudebug x86_64 package](https://container-obsfs-filesystem.obs.cn-north-4.myhuaweicloud.com/package/cann/asc-toolkit-dev/version_compile/master/202601/20260131_174635_/ubuntu_x86/cann-asc-tools-cpudebug-deps-lib_debug_9.0.0_linux-x86_64.tar.gz), [cpudebug aarch64 package](https://container-obsfs-filesystem.obs.cn-north-4.myhuaweicloud.com/package/cann/asc-toolkit-dev/version_compile/master/202601/20260131_174635_/ubuntu_aarch64/cann-asc-tools-cpudebug-deps-lib_debug_9.0.0_linux-aarch64.tar.gz)). The Debug version is more convenient for debugging compared to the Release version.

  You need to create a `{your_3rd_party_path}` directory in the environment to store the dependent repository packages, closed-source packages, and third-party open-source software packages.

  ```bash
  mkdir -p {your_3rd_party_path}
  ```

  After creating the directory, upload the downloaded dependency packages to the `{your_3rd_party_path}` directory, then use the following command to run UT tests:

  ```bash
  bash build.sh --test --cann_3rd_lib_path={your_3rd_party_path}
  ```

### UT Test Coverage Display

- Dependencies
    - lcov >= 1.14

- Run command

  ```bash
  bash build.sh --test --cov
  ```
