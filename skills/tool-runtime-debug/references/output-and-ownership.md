# 输出与所有权

## 何时阅读

当 Host Runtime 或 ACLNN 调用通过输出参数返回值、句柄或地址，且值为空、未初始化、类型不符或后续消费者不一致时阅读本页。

若对象是 SIMT TensorList 描述，本页只追踪输出地址、描述对象及其消费者，不推断 TensorList 布局或 VF 规则。证据转向 Kernel 解析后，保留 Host 侧对象关系并停止本专题。

## 排查原则

追踪 API、输出参数位置、实际声明、初值、所有者和后续消费者。输出参数需指向类型匹配的可写存储；读取前先核对调用的成功条件，不能仅凭指针非空判断容量、生命周期或数据正确。

## 版本经验

以下标签仅约束各自条目，不代表整篇或整个 Skill 的适用范围。`official_product_support_table` 严格以条目所引官方页面为准。

### Host 分配通过输出参数返回未初始化内存

```yaml
applies_to: soc=official_product_support_table; cann=9.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

`aclrtMallocHost` 的输出参数是存放分配地址的 `void **`，`size` 使用 bytes；分配内存未初始化。此项不验证 ACLNN Tensor 所有权。下列 8.x ACLNN 经验仍按各自范围使用，9.0.0 的具体 ACLNN API 须另查同版本文档，不能由 Host 分配接口类推。

文档依据：`S-OUTPUT-900`。

### Tensor 创建时绑定用户数据地址

```yaml
applies_to: soc=official_product_support_table; cann=8.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

Tensor 描述对象持有传入的 device 数据地址；描述对象与数据地址须分别追踪。

依据：`S-TENSOR-800` 中的 aclCreateTensor 文档（官方文档归纳，未建立本条实测对应）。

### Tensor 描述对象的销毁不替代数据释放

```yaml
applies_to: soc=official_product_support_table; cann=8.5.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

销毁 Tensor 描述对象不替代用户数据的 `aclrtFree`；须另行确认数据所有者及最后一次使用。

依据：`S-TENSOR-800` 中的 aclDestroyTensor 文档（官方文档归纳，未建立本条实测对应）。

## 最小检查顺序

1. 从最早失败的调用记录返回码和输出参数的实际存储位置。
2. 对照目标头文件检查参数类型、指针层级和成功条件。
3. 沿赋值追到地址的分配点，记录所有者及每一个实际使用点。
4. 若结果经 D2H 读回，比较输出描述对象绑定的 device 地址、读回源地址和分配记录；不一致时只把它作为当前假设的候选证据。
5. 用同一复现命令核对输出数据或句柄的实际用途，再决定是否修改。

## 待验证假设与常见误判

- 输出描述对象绑定地址与 D2H 源地址相同，只适用于直接输出的具体数据流；先核对代码，不把它推广到所有 ACLNN 操作。
- 指针非空不能证明内存容量足够、仍有效或属于当前调用方。
- 返回码成功不能证明后续读取了正确的输出缓冲区。
- 未找到分配点、外部所有者或实际声明时，标为 `UNRESOLVED`，不要推测类型或补加释放操作。

## 验证与退出

保持验证器、期望值和返回语义不变。一次只修复有证据支持的绑定或输出参数，再以同一命令检查结果。若最早失败已转向其他领域，停止 Runtime 排查并记录中性领域名称。

## 官方版本定位

本页的核心接口是 `aclCreateTensor`、`aclDestroyTensor` 和通过输出参数返回地址的分配接口。容量、拷贝方向和 workspace 交接见 [内存与容量](memory-and-capacity.md)。官方页面中有产品支持表时，产品范围以该表为准。

| CANN 版本 | 官方文档 | 本页需核对的差异 |
| --- | --- | --- |
| 8.0.0 | [`aclCreateTensor`](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/aolapi/operatorlist_00019.html)、[`aclrtMallocHost`](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0101.html) | 按 8.0.0 的 tensor 绑定和输出参数约定处理。 |
| 8.1.RC1.alpha001 | [`aclCreateTensor`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha001/apiref/aolapi/operatorlist_00019.html)、[`aclrtMallocHost`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha001/apiref/appdevgapi/aclcppdevg_03_0101.html) | 不从 8.0.0 外推，核对本版本原型与产品支持表。 |
| 8.2.RC1.alpha001 | [`aclCreateTensor`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/aolapi/operatorlist_00019.html)、[`aclrtMallocHost`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/appdevgapi/aclcppdevg_03_0101.html) | 核对 tensorData 绑定和 Host 分配输出约定。 |
| 8.3.RC1.alpha001 | [`aclCreateTensor`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/83RC1alpha001/API/aolapi/operatorlist_00019.html)、[`aclrtMallocHost`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/83RC1alpha001/API/appdevgapi/aclcppdevg_03_0101.html) | 核对当版 API 列表和产品限制。 |
| 8.5.0 | [`aclCreateTensor`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/aolapi/operatorlist_00019.html)、[`aclDestroyTensor`](https://www.hiascend.com/document/detail/en/canncommercial/850/API/aolapi/operatorlist_00027.html)、[`aclrtMallocHost`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/appdevgapi/aclcppdevg_03_0101.html) | 销毁描述对象不代替释放用户绑定的 device 数据。 |
| 9.0.0 | [`aclCreateTensor`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/aolapi/operatorlist_00019.html)、[`aclrtMallocHost`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/runtimeapi/aclcppdevg_03_0101.html) | `tensorData` 页面明确给出地址对齐要求；以本版页面为准。 |
| 9.1.0 | [`aclCreateTensor`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/910/API/aolapi/operatorlist_00019.html) | 公开页面列出 Ascend 950PR/950DT 支持；不把系列名替换为当前 9589 子型号。 |
| 9.2.0-beta.1 | [`aclCreateTensor`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta1/API/aolapi/operatorlist_00019.html) | beta 版单独核对，不写成 9.2.0 正式版。 |
| 9.2.0-beta.2 | [`aclCreateTensor`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta2/API/aolapi/operatorlist_00019.html) | 当前可核对的最新公开 beta 页面；不代表 9.3.0。 |
| 9.3.0 | 未找到对应的官方公开页面 | 当前 950 环境安装此版本；必须查安装头文件和实际行为。 |

## 来源

### CANN 9.0.0

- `S-OUTPUT-900` — [aclrtMallocHost，CANN 商用版 9.0.0](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0101.html)

  - API：`aclrtMallocHost`；文档版本：CANN 商用版 9.0.0，产品范围按页面支持表。
  - 引用内容：Host 分配通过输出参数返回未初始化内存。

### 其他版本的文档依据

- `S-TENSOR-800` — [aclCreateTensor，CANN 商用版 8.0.0](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/aolapi/operatorlist_00019.html)；[aclDestroyTensor，CANN 商用版 8.5.0](https://www.hiascend.com/document/detail/en/canncommercial/850/API/aolapi/operatorlist_00027.html)

  - API：`aclCreateTensor`、`aclDestroyTensor`、`aclTensor`、`tensorData`。
  - 文档版本：分别为 CANN 商用版 8.0.0 与 8.5.0。
  - 引用内容：Tensor 描述对象与用户数据分别管理。
