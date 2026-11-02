# Preparation

<!-- md-trans-meta sourceCommit=681b3fab3b2117829dc3976c9606bfc0f3d15012 translatedAt=2026-10-14T08:54:25.791Z pushedAt=2026-10-15T11:15:08.875Z -->

## Environment Setup

1. Install the software packages. See *CANN Software Installation* to install the driver, firmware, and CANN software packages.
2. Configure the environment variables.

    CANN provides process-level environment variable setting scripts for users to reference in processes, so that environment variables are set automatically. The following shows example commands. The examples use the default installation paths for both root and non-root users. Replace them with the actual installation paths.

    ```bash
    # Configure the environment variables after installing the toolkit package as the root user.
    source /usr/local/Ascend/cann/set_env.sh
    # Configure the environment variables after installing the toolkit package as a non-root user.
    source ${HOME}/Ascend/cann/set_env.sh
    ```

3. For debug version compilation, developers need to install the binary packages actually used when the issue occurs, including static binary packages and dynamic binary packages.

## Obtaining the Tool

The tool is located in the `${INSTALL_DIR}/bin/op_compiler` directory.

Replace `${INSTALL_DIR}` with the file storage path after CANN software installation. For example, if you install the software as the **root** user, the default storage path is `/usr/local/Ascend/cann`.
