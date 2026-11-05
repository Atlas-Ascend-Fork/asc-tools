# Installing and Uninstalling a Kernel Package

<!-- md-trans-meta sourceCommit=dab20c1a5c0505cf777deac6d50259bef05c16c7 translatedAt=2026-10-30T10:08:02.898Z pushedAt=2026-11-03T00:55:22.822Z nl=2 -->

The following uses a kernel package with the default name as an example. During actual execution, replace the kernel package name in the commands with the actual one.

1. Install the static kernel package.

    Go to the directory where the `static_kernel_${datetime}_${pid}.run` package is located, and execute the RUN package as the running user (for example, **HwHiAiUser**):

    ```bash
    ./static_kernel_${datetime}_${pid}.run
    ```

    The following message indicates that the installation is successful.

    ```text
    Verifying archive integrity...  100%   SHA256 checksums are OK. All good.
    Uncompressing STATIC KERNEL RUN PACKAGE  100%
    ```

    Both default installation and installation to a specified directory are supported.

    - Default installation: The RUN package is installed to the `${INSTALL_DIR}/opp/static_kernel` path by default, where `${INSTALL_DIR}` is the file storage path after the CANN software is installed. Source the CANN software environment variables based on the actual situation before performing the default installation.

    - Installation to a specified directory: You can customize the installation target path by using the **--install-path=<path\>** parameter.

    > [!NOTE] Note
    >- The default permission of the RUN package installation path `${INSTALL_DIR}/opp/static_kernel` is 770 (accessible to the current user and users in the same group). If the static kernel package fails to be installed due to insufficient permissions, contact the user who installed the CANN software package to modify the permission of the `static_kernel` directory.
    >- The RUN package supports silent installation, which can be enabled by using the **--quiet** parameter (abbreviated as **-q**).

    The following is an example of the directory structure after the RUN package is installed:

    ```text
    |-- ${install_path}/opp/static_kernel
        |-- ai_core
            |-- config
               |-- ascendxxxx
               |    |-- binary_info_config.json            # Total index of all static kernel packages
               |    |-- shape_info                         # Directory of original shape files
               |-- static_kernel.lock                      # File lock
            |-- config.ini                               # Configuration file that records the installation order.
            |-- static_kernel_250924103634186357_988879            # Static kernel file with timestamp "250924103634186357" and process ID "988879"
               |-- ascendxxxx
               |   |-- Add                                 # Directory of operator binary files
               |      |-- static_kernel_Add_float16_NCL_xxxx_d0.json     # d0 indicates that the determinism switch is off
               |      |-- static_kernel_Add_float16_NCL_xxxx_d1.json     # d1 indicates that the determinism switch is on
               |      |-- static_kernel_Add_float16_NCL_xxxx_d0.o
               |      |-- static_kernel_Add_float16_NCL_xxxx_d1.o
               |   |-- xxxx
               |      |-- static_kernel_xxx.json
               |      |-- static_kernel_xxx.o
               |   |-- ......
               |-- config                                # Index of a single static kernel package
               |   |-- ascendxxxx
               |       |-- binary_info_config.json
               |       |-- shape_info                    # Directory of original shape files
               |-- scripts                               # Common scripts used by the tool
               |   |-- ......
               |-- uninstall.sh                        # Single-package uninstall script
            |-- static_kernel_xxxx                    # Static kernel files with different timestamps
            |-- uninstall.sh                           # Full uninstall script
            |-- version.info                             # Version information
    ```

    > [!NOTE] Note
    > Multiple kernel packages can be installed. If multiple packages contain the same operator kernel, the kernel package installed later takes effect.

2. (Optional) When the static kernel package is no longer needed, you can uninstall a single package or uninstall all packages.
    - Uninstalling a single package

        Go to the installation directory of the `static_kernel_${datetime}_${pid}.run` package and run **uninstall.sh** as the running user (for example, **HwHiAiUser**).

        ```bash
        cd ${install_path}/opp/static_kernel/ai_core/static_kernel_${datetime}_${pid}
        ./uninstall.sh
        ```

        If the uninstallation succeeds, the `static_kernel_${datetime}_${pid}` folder under the `ai_core` directory will be deleted.

    - Uninstalling all packages

        Go to the `${install_path}/opp/static_kernel/ai_core` directory and run **uninstall.sh** as the running user (for example, **HwHiAiUser**).

        ```bash
        cd ${install_path}/opp/static_kernel/ai_core/
        ./uninstall.sh
        ```

        After the uninstallation succeeds, all content in the `ai_core` directory except the `config/static_kernel.lock` file will be deleted, and all installed kernel packages are uninstalled.
