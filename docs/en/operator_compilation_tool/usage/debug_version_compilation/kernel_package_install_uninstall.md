# Installing and Uninstalling a Kernel Package

<!-- md-trans-meta sourceCommit=dab20c1a5c0505cf777deac6d50259bef05c16c7 translatedAt=2026-10-30T10:07:40.726Z pushedAt=2026-11-03T00:55:22.820Z nl=2 -->

The following uses the kernel package with the default name as an example. During actual execution, replace the kernel package name in the commands with the actual one.

- Install the debug-version kernel package.

    Go to the directory where the `debug_kernel_${datetime}.run` package is located, and execute the RUN package as the running user (for example, **HwHiAiUser**):

    ```bash
    ./debug_kernel_${datetime}.run
    ```

    The following message indicates that the installation is successful.

    ```text
    installation complete.
    ```

    Currently, installation to a specified directory is not supported. The RUN package is installed to the `${install_path}/opp/debug_kernel` directory by default, where `${install_path}` is the storage path of the files after CANN software installation.

    > [!NOTE] Note
    > The default permission of the RUN package installation directory `${install_path}/opp/debug_kernel` is 755. If the debug-version kernel package fails to be installed due to insufficient permissions, contact the user who installed the CANN software package to modify the permission of the `debug_kernel` directory.

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
