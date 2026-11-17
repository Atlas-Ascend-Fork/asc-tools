# Security Statement

## Recommended Runtime User

For security purposes, do not use administrator accounts such as root to run any command. Follow the principle of least privilege.

## File Permission Control

- It is recommended that you set the system umask to 0027 or higher on the host (including the host machine) and in containers. This ensures that new folders have a default maximum permission of 750 and new files have a default maximum permission of 640.
- It is recommended that you apply permission control and other security measures to sensitive content such as personal privacy data, commercial assets, source files, and various files saved during code development. For example, this involves permission control over the installation directory of this project and over public input data files. For the recommended permission settings, see [A-Recommended Maximum Permissions for Files (Folders) in Different Scenarios](#a-recommended-maximum-permissions-for-files-folders-in-different-scenarios).
- Apply proper permission control during installation and usage. Set permissions by referring to [A-Recommended Maximum Permissions for Files (Folders) in Different Scenarios](#a-recommended-maximum-permissions-for-files-folders-in-different-scenarios).

## Build Security Statement

When you build and install this project from source code, you need to compile it by yourself. The compilation process generates intermediate files. It is recommended that you apply proper permission control to the intermediate files after compilation to ensure file security.

## Runtime Security Statement

- The process exits and prints an error message when a runtime exception occurs. Locate the specific cause based on the error message.

## Public Network Address Statement

The public network addresses contained in the code of this project are as follows:

|      Type      |                                           Open-Source Code Address                                           |                            File Name                             |             Public IP Address/Public URL/Domain Name/Email Address/Archive Address             |                   Description                    |
| :------------: |:------------------------------------------------------------------------------------------:|:----------------------------------------------------------| :---------------------------------------------------------- |:-----------------------------------------|
|  Dependency  | N/A  | cmake/third_party/gtest.cmake | <https://gitcode.com/cann-src-third-party/googletest/releases/download/v1.14.0/googletest-1.14.0.tar.gz> | Downloads the googletest source code from GitCode as a compilation dependency. |

---

## Vulnerability Mechanism Description

[Vulnerability Management](https://gitcode.com/cann/community/blob/master/security/security.md)

## Appendix

### A-Recommended Maximum Permissions for Files (Folders) in Different Scenarios

| Type           | Maximum Reference Linux Permission |
| -------------- | ---------------  |
| Home directory                        |   750 (rwxr-x---)            |
| Program files (including script files, library files, and so on)       |   550 (r-xr-x---)             |
| Program file directories                      |   550 (r-xr-x---)            |
| Configuration files                          |  640 (rw-r-----)             |
| Configuration file directories                      |   750 (rwxr-x---)            |
| Log files (recording completed or archived)        |  440 (r--r-----)             |
| Log files (being recorded)                |    640 (rw-r-----)           |
| Log file directories                      |   750 (rwxr-x---)            |
| Debug files                         |  640 (rw-r-----)         |
| Debug file directories                     |   750 (rwxr-x---)  |
| Temporary file directories                      |   750 (rwxr-x---)   |
| Maintenance and upgrade file directories                  |   770 (rwxrwx---)    |
| Service data files                      |   640 (rw-r-----)    |
| Service data file directories                  |   750 (rwxr-x---)      |
| Key components, private keys, certificates, and ciphertext file directories    |  700 (rwx—----)      |
| Key components, private keys, certificates, and encrypted ciphertext        | 600 (rw-------)      |
| Encryption/decryption APIs and scripts            |   500 (r-x------)        |
