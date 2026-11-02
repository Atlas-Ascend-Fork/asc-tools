# Installing and Uninstalling a Kernel Package

<!-- md-trans-meta sourceCommit=681b3fab3b2117829dc3976c9606bfc0f3d15012 translatedAt=2026-10-14T08:56:18.779Z pushedAt=2026-10-15T11:15:08.877Z -->

The following uses the kernel package with the default name as an example. During actual execution, replace the kernel package name in the commands with the actual one.

- Install the debug-version kernel package.

    Go to the directory where the **debug\_kernel\_*$\{datetime\}*.run** package is located, and execute the RUN package as the running user (for example, **HwHiAiUser**):

    ```bash
    ./debug_kernel_${datetime}.run
    ```

    The following message indicates that the installation is successful.

    ```text
    installation complete.
    ```

    Currently, installation to a specified directory is not supported. The RUN package is installed to the ***$\{install\_path\}*/opp/debug\_kernel** path by default, where ***$\{install\_path\}*** is the file storage path after the CANN software is installed.

    > [!NOTE] Note
    > The default permission of the default installation path ***$\{install\_path\}*/opp/debug\_kernel** for the RUN package is 755. If the debug-version kernel package fails to be installed due to insufficient permissions, contact the user who installed the CANN software package to modify the permission of the **debug\_kernel** directory.

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

    Go to the installation directory of the **debug\_kernel\_*$\{datetime\}*.run** package and run **uninstall.sh** as the running user (for example, **HwHiAiUser**).

    ```bash
    cd ${install_path}/opp/debug_kernel
    ./uninstall.sh
    ```

    If the uninstallation succeeds, the **debug\_kernel** folder will be deleted.
