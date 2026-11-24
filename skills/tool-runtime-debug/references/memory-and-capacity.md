# 内存与容量

## 何时阅读

当线性拷贝的方向、目的地容量、分配大小或 ACLNN workspace 交接与实际结果不符时阅读本页。

## 排查原则

先根据源、目的指针的实际地址空间确认拷贝方向，再区分元素数量与 bytes。地址空间需追溯分配调用或调用方约定，不能从变量名或方向枚举反推。只在目标 API 明确采用 bytes 时比较请求量、声明容量和已分配范围。ACLNN 其他算子不能直接套用 Abs 示例。

## 处理 `507035` 时只做 Host 边界核对

UB 起始地址对齐、DataCopy/DataCopyPad 参数和 UB 越界属于 Kernel/AI Core 检查方向，不在 Host Runtime 专项里直接修改。本页先核对以下 Host 边界：

- Host 分配的 device/workspace 容量是否覆盖 Kernel 参数声明的实际访问范围；
- `destMax`、`count`、workspace size 和 shape 乘法是否均以 bytes 计算，且无整数溢出；
- 传入 Kernel 的地址是否就是被分配、未释放且与当前 Context 匹配的地址。

上述 Host 边界成立后，将原始 `507035`、首个异常任务和完整输入交给 AI Core；不在本页继续修改 Kernel。

## 版本经验

以下标签仅约束各自条目，不代表整篇或整个 Skill 的适用范围。`official_product_support_table` 严格以条目所引官方页面为准。

### 同步复制的容量与方向判定

```yaml
applies_to: soc=official_product_support_table; cann=9.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

`aclrtMemcpy` 的 `destMax` 与 `count` 以 bytes 计；同步接口的 `kind` 为预留参数、不参与方向配置，须结合实际指针核对复制是否合法；该接口不隐式同步。不把 8.2 枚举定义当作 9.0.0 同步 memcpy 的实际执行规则，也不能将此结论套用于异步接口。

文档依据：`S-MEMORY-900`。

### 复制容量与请求长度以字节计

```yaml
applies_to: soc=official_product_support_table; cann=8.5.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

`aclrtMemcpy` 的 `destMax` 和 `count` 均以 bytes 解释。

依据：`S-MEMCPY-850`（官方文档归纳，未建立本条实测对应）。

### 拷贝方向枚举与地址空间

```yaml
applies_to: soc=official_product_support_table; cann=8.2.RC1.alpha001; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

`aclrtMemcpyKind` 枚举说明源与目的的地址空间；目标版本仍须核对本机头文件与实际使用该枚举的 API，枚举定义本身不能证明接口执行时采用该参数。

依据：`S-MEMCPY-KIND-82A1`（官方文档归纳，未建立本条实测对应）。

### Device 分配的输出、单位与释放配对

```yaml
applies_to: soc=official_product_support_table; cann=8.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

`aclrtMalloc` 的 `size` 采用 device bytes，成功后由输出参数返回地址，并与 `aclrtFree` 配对。

依据：`S-MALLOC-800`（官方文档归纳，未建立本条实测对应）。

### Host 分配的容量与释放配对

```yaml
applies_to: soc=official_product_support_table; cann=8.5.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

`aclrtMallocHost` 的 `size` 采用 Host bytes，返回的分配与 `aclrtFreeHost` 配对。9.0.0 以后的释放选项见[资源生命周期](resource-lifecycle.md)，不把本条列举当作新版本的完整集合。

依据：`S-MALLOC-HOST-850`（官方文档归纳，未建立本条实测对应）。

### Abs 示例中的 workspace 交接

```yaml
applies_to: soc=official_product_support_table; cann=8.5.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

示例以查询 bytes 分配 workspace，并将地址、大小、executor 和 stream 传给执行调用；其他算子须查自身声明。

依据：`S-ACLNN-FLOW-850`（官方文档归纳，未建立本条实测对应）。

## 最小检查顺序

1. 记录最早失败的复制或分配调用及其实际源/目的指针，依据两端的分配来源确认 Host/Device 地址空间及拷贝方向；来源缺失时明确要求补证据。
2. 从目标头文件读取参数签名、容量单位和拷贝类型枚举。
3. 沿目的地址追到分配点，比较分配 bytes、调用声明的最大容量和请求 bytes；检查大小运算是否溢出。
4. 对 workspace，逐点比较查询、分配和执行处的指针、大小、executor。
5. 只修复第一处由实际证据确认的不一致，以原命令检查完整数据范围。

## 待验证假设与常见误判

- 单一版本的线性 copy 文档不能证明 pitch、2D copy 或 memset 的参数单位与末行边界。遇到这些 API，核对目标声明和对应版本文档。
- 不从变量名推断地址空间、所有者或分配长度；需要分配调用或调用方约定作为证据。
- 不因一次 API 调用返回成功就推断方向正确或目标缓冲区足够大。
- 错误方向是否返回成功属于版本和场景相关现象。没有目标证据时只把它作为假设，不据此改多个参数。

## 验证与退出

保留原拷贝大小、比较范围和验证器语义。修改后以同一命令观察实际输出及完整字节范围。若容量依赖未知尺寸计算或外部所有者，报告 `UNRESOLVED`，不增加任意 padding。

## 已有实测经验：目的容量必须覆盖复制长度

```yaml
applies_to: soc=Ascend910B1_V1; cann=9.0.0; bisheng=15.0.5-5c68a1cb1231; op_class=quant
verified_on: soc=Ascend910B1_V1; cann=9.0.0; bisheng=15.0.5-5c68a1cb1231; op_class=quant
```

证据标识：`runtime-memcpy-canary-910-20260916-v2 / RT165-project`。该固定量化案例将 `destMax` 从 `kOutputBytes - 1` 修为 `kOutputBytes`，保留复制长度和比较逻辑，三次记录均为 `d2h_status=0`、完整输出匹配参考。这只证明该容量修复样例，不证明错误 kind 的行为、其他算子或整个技能有效性。

## 官方版本定位

引用拷贝或分配文档时，同时说明容量、方向和字节单位仍须逐项对照目标版本页面及实际源码参数，不能由链接本身确认本次调用正确。

本页的核心接口是 `aclrtMalloc`、`aclrtMallocHost`、`aclrtMemcpy` 和 `aclrtMemcpyKind`。同步 `aclrtMemcpy` 与异步接口的 `kind`、完成边界不可混用。

| CANN 版本 | 分配文档 | 复制文档 | 差异处理 |
| --- | --- | --- | --- |
| 8.0.0 | [`aclrtMalloc`](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0095.html)、[`aclrtMallocHost`](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0101.html) | [`aclrtMemcpy`](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0105.html) | 按 8.0.0 核对 allocator 族、bytes 单位和 `kind`。 |
| 8.1.RC1.alpha001 | [`aclrtMalloc`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha001/apiref/appdevgapi/aclcppdevg_03_0095.html) | [`aclrtMemcpy`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha001/apiref/appdevgapi/aclcppdevg_03_0105.html) | 不用 8.0.0 的产品范围替代本版支持表。 |
| 8.2.RC1.alpha001 | [`aclrtMalloc`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/appdevgapi/aclcppdevg_03_0095.html) | [`aclrtMemcpy`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/appdevgapi/aclcppdevg_03_0105.html)、[`aclrtMemcpyKind`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/appdevgapi/aclcppdevg_03_1807.html) | 枚举说明不等于每个接口都使用该参数决定方向。 |
| 8.3.RC1.alpha001 | [`aclrtMalloc`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/83RC1alpha001/API/appdevgapi/aclcppdevg_03_0095.html) | [`aclrtMemcpy`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/83RC1alpha001/API/appdevgapi/aclcppdevg_03_0105.html) | 核对当版同步接口的方向判定和产品限制。 |
| 8.5.0 | [`aclrtMalloc`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/appdevgapi/aclcppdevg_03_0095.html)、[`aclrtMallocHost`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/appdevgapi/aclcppdevg_03_0101.html) | [`aclrtMemcpy`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/appdevgapi/aclcppdevg_03_0105.html) | `destMax` 和 `count` 均以 bytes 计；其他 copy 家族另查对应页面。 |
| 9.0.0 | [`aclrtMalloc`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/runtimeapi/aclcppdevg_03_0095.html)、[`aclrtMallocHost`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/runtimeapi/aclcppdevg_03_0101.html) | [`aclrtMemcpy`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/runtimeapi/aclcppdevg_03_0105.html) | 当前页面将同步接口的 `kind` 标为预留，由实际地址决定合法性；不外推到异步接口。 |
| 9.1.0 | [`aclrtMalloc`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/runtimeapi/aclcppdevg_03_0095.html)、[`aclrtMallocHost`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/runtimeapi/aclcppdevg_03_0101.html) | [`aclrtMemcpy`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/runtimeapi/aclcppdevg_03_0105.html) | 核对页面中 Ascend 950PR/950DT 支持和产品形态差异。 |
| 9.2.0-beta.1 | [`aclrtMalloc`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta1/API/runtimeapi/aclcppdevg_03_0095.html) | [`aclrtMemcpy`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta1/API/runtimeapi/aclcppdevg_03_0105.html) | beta.1 单独核对，不合并成 9.2.0 正式版。 |
| 9.2.0-beta.2 | [`aclrtMalloc`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta2/API/runtimeapi/aclcppdevg_03_0095.html) | [`aclrtMemcpy`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta2/API/runtimeapi/aclcppdevg_03_0105.html) | 当前最新可核对公开 beta 页面。 |
| 9.3.0 | 未找到对应的官方公开页面 | 未找到对应的官方公开页面 | 当前 950 环境只能用安装头文件和实际运行证据确认。 |

## 来源

### CANN 9.0.0

- `S-MEMORY-900` — [aclrtMemcpy](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0105.html)、[aclrtMallocHost](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0101.html)，CANN 商用版 9.0.0

  - API：`aclrtMemcpy`、`aclrtMallocHost`；文档版本：9.0.0，产品范围按页面支持表。
  - 引用内容：同步复制的容量与方向判定。

### 其他版本的文档依据

- `S-MEMCPY-850` — [aclrtMemcpy，CANN 社区版 8.5.0](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/appdevgapi/aclcppdevg_03_0105.html)

  - API：`aclrtMemcpy`；参数 `destMax`、`count`。
  - 文档版本：CANN 社区版 8.5.0；产品差异须另行核对。
  - 引用内容：复制容量与请求长度以字节计。

- `S-MEMCPY-KIND-82A1` — [aclrtMemcpyKind，CANN 8.2.RC1.alpha001](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/appdevgapi/aclcppdevg_03_1807.html)

  - API/枚举：`aclrtMemcpy`、五个 `ACL_MEMCPY_*` 枚举。
  - 文档版本：CANN 社区版 8.2.RC1.alpha001。
  - 引用内容：拷贝方向枚举与地址空间。

- `S-MALLOC-800` — [aclrtMalloc，CANN 商用版 8.0.0](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0095.html)

  - API：`aclrtMalloc`、输出参数和 `size`。
  - 文档版本：CANN 商用版 8.0.0。
  - 引用内容：Device 分配的输出、单位与释放配对。

- `S-MALLOC-HOST-850` — [aclrtMallocHost，CANN 商用版 8.5.0](https://www.hiascend.com/document/detail/en/canncommercial/850/API/appdevgapi/aclcppdevg_03_0101.html)

  - API：`aclrtMallocHost`、输出参数和 `size`。
  - 文档版本：CANN 商用版 8.5.0。
  - 引用内容：Host 分配的容量与释放配对。

- `S-ACLNN-FLOW-850` — [Quick Start，CANN 商用版 8.5.0](https://www.hiascend.com/document/detail/en/canncommercial/850/quickstart/instg_quick.html)

  - API：`aclnnAbsGetWorkspaceSize`、`aclnnAbs`。
  - 文档版本：CANN 商用版 8.5.0 的示例。
  - 引用内容：Abs 示例中的 workspace 交接。
