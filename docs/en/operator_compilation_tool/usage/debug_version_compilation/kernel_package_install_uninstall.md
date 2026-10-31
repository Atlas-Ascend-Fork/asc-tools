# Installing and Uninstalling a Kernel Package

<!-- md-trans-meta sourceCommit=2575ba72def56b10b4e28610e3f0129db5bc2da9 translatedAt=2026-10-15T11:28:52.930Z pushedAt=2026-10-16T01:20:29.057Z -->

The following uses the kernel package with the default name as an example. During actual execution, replace the kernel package name in the commands with the actual one.

- Install the debug-version kernel package.

    Go to the directory where the `debug_kernel_${datetime}.run` package is located and run the RUN package as the running user (for example, **HwHiAiUser**):

    ```bash
    ./debug_kernel_${datetime}.run
    ```

    The following message indicates that the installation is successful.

    ```text
    installation complete.
    ```

    Currently, installation to a specified directory is not supported. The RUN package is installed to the `${install_path}/opp/debug_kernel` path by default, where `${install_path}` is the file storage path after the CANN software is installed.

    > [!NOTE] Note
    > The default permission of the default installation path `${install_path}/opp/debug_kernel` for the RUN package is 755. If the debug-version kernel package fails to be installed due to insufficient permissions, contact the user who installed the CANN software package to change the permission of the `debug_kernel` directory.

    The following is an example of the directory structure after the RUN package is installed:

    ```text
    |-- ${install_path}/opp/debug_kernel
        |-- ascendxxxx
        |   |-- gelu # Operator binary directory
        |       |-- Gelu_5c5e58e043224cccf51a06108010ae13_high_precision.json
        |       |-- Gelu_5c5e58e043224cccf51a06108010ae13_high_precision.o
        |-- config
        |   |-- ascendxxxx
        |       |-- binary_info_config.json  # Total index of the full kernel package
        |       |-- gelu.json # Index of the operator kernel
        |-- scripts # Common scripts used by the tool
        |   |-- common.py
        |   |-- help.info
        |   |-- install.py
        |   |-- uninstall.py
        |-- uninstall.sh  # Uninstallation script
        |-- version.info # Version information
    ```

    > [!NOTE] Note
    >When multiple kernel packages are installed, the kernel package installed later overwrites the previously installed kernel package.

- Uninstallation

    Go to the installation directory of the `debug_kernel_${datetime}.run` package and run **uninstall.sh** as the running user (for example, **HwHiAiUser**).

    ```bash
    cd ${install_path}/opp/debug_kernel
    ./uninstall.sh
    ```

    If the uninstallation is successful, the `debug_kernel` folder will be deleted.
