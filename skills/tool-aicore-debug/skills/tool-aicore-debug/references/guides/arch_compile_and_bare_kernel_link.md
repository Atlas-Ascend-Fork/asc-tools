# 芯片架构映射、唯一注册入口与裸设备核函数编译链接检查指南

## 一、触发指纹与子例对比

- **典型报错**：`0x10`（`illegal instruction` 非法指令）、`0x8000`（标量异常），或修改源码重新运行后表现毫无变化（改到了未生效的备份源码或非当前芯片代际目录）。
- **过/崩子例对比**：
  1. 源码同时存在 `arch22/`（或 `dav-c220`）与 `arch35/`（或 `dav-c310`）多套实现，改错代际目录导致实卡不生效；
  2. 采用裸设备二进制加载（`aclrtBinaryLoadFromFile` / `binary_load_from_file`）的场景，直接将 `bisheng -x asc -c` 产出的 `.o` 加载上板，必报 `0x10` 或 `0x8000`。
- **30 秒源码搜索词**：在算子源码中搜 `IMPL_OP`、`OpDef`、`SetTiling`、`__NPU_ARCH__`、`--cce-aicore-arch`、`--cce-aicore-only`、`ld.lld`、`binary_load`。

## 二、必查参数与公式清单

1. **直接从算子源码锁定唯一生效入口（不假设存在特定构建脚本）**：
   - 算子目录唯一确定存在的是源码文件：直接搜 `IMPL_OP`、`OpDef`、`SetTiling` 及核函数模板分发调用链，确认真正绑定生效的主控与核函数源文件，跳过同目录下未被任何入口引用的旧文件（仅当目录内附带构建配置时才顺带核对源文件列表）。
2. **运行芯片型号到架构宏与源码目录映射表**：
   - `Ascend 910B / Atlas A2`、`Ascend 910_93 / Atlas A3`：`--cce-aicore-arch=dav-c220`（分离核含 `dav-c220-vec` / `dav-c220-cube`），源码宏 `__NPU_ARCH__ == 2201`，对应 `dav-c220` / `arch22` 目录；
   - `Ascend 950 / Atlas A5`：`--cce-aicore-arch=dav-c310`，源码宏 `__NPU_ARCH__ == 3510`，对应 `dav-c310` / `arch35` 目录；
   - `Ascend 310P`：`dav-m200`；`Ascend 310B`：`dav-m300`；
   - 跨代专属字段（如 `FixpipeConfig.isToUB` 仅 `3510/5102` 存在、`GetUBSizeInBytes()` 在 `2201` 受守卫）须加 `#if __NPU_ARCH__ == ...` 编译期隔离。
3. **裸设备核函数编译与重定位链接两步约束（仅适用于裸核加载场景，防 `0x10`）**：
   - 常规算子工程只修改算子源码，严禁自行新建或改动构建脚本；
   - 仅当工程明确采用直接调用 `bisheng` 编译纯设备核函数并由运行时加载二进制文件时，`bisheng` 默认产出的混合对象（含 AArch64 宿主桩）不能直接加载，须在现有编译命令中配齐 `--cce-aicore-only` 与 `ld.lld -m aicorelinux -Ttext=0`。
4. **区分算子缺陷与硬件物理故障**：日志出现 ECC 多比特不可纠错、奇偶校验或硬件总线 CRC 物理告警时，属设备硬件 RAS 事件；编译器已隐式保证的基础标量顺序依赖无需手写内联汇编。

## 三、常见误区（避坑）

- **误区 1**：一上来假设目录里一定有编译脚本去搜改脚本，或在标准算子工程里乱改构建文件；应始终以算子源码注册链路为准。
- **误区 2**：裸核加载场景只加 `--cce-aicore-only -c` 漏掉 `ld.lld -m aicorelinux -Ttext=0` 重定位链接；或在无符号环境反复运行同样的反汇编命令却不补充符号、产物信息或更换取证方法。

## 四、标准修复代码模板

```cpp
// 1. 源码内跨芯片代际字段编译宏隔离模板
#if defined(__NPU_ARCH__) && (__NPU_ARCH__ == 3510 || __NPU_ARCH__ == 5102)
    fixpipeConfig.isToUB = true;
#endif
```

```bash
# 2. 仅限已存在裸核 bisheng 编译命令的场景：补齐纯设备编译与重定位链接参数
bisheng -x asc --cce-aicore-arch=dav-c220-vec --cce-aicore-only -O2 -c kernel.cpp -o kernel_aicore.o
ld.lld -m aicorelinux -Ttext=0 kernel_aicore.o -o kernel.o
```
