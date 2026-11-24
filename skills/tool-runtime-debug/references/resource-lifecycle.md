# 资源生命周期

## 何时阅读

当释放时崩溃、怀疑重复释放、使用已释放地址，或资源仍被异步任务使用时阅读本页。

若问题属于 Blaze Launcher 的 Host 资源清理，本页只追踪实际 owner、最后消费者和完成边界，不推断 Blaze 专用顺序。证据转向 Kernel drain 或跨核同步后，停止本专题并重新分诊。

## 排查原则

对每个资源记录 `producer -> owner -> consumers -> completion boundary -> cleanup`。释放 API 必须匹配实际分配族；所有者不明时，不添加猜测性释放。对象销毁顺序须结合目标版本和实际消费者确认。

## 版本经验

以下标签仅约束各自条目，不代表整篇或整个 Skill 的适用范围。`official_product_support_table` 严格以条目所引官方页面为准。

### 普通 Host 释放与带同步释放不能混用语义

```yaml
applies_to: soc=official_product_support_table; cann=9.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

FreeHost 接受 MallocHost/WithCfg 的分配，不隐式同步；MallocHost 文档另列 FreeHostWithDevSync。普通释放与显式带同步的变体不能混称为“均不等待”。

文档依据：`S-FREE-HOST-900`。

### Device 分配的输出、单位与释放配对

```yaml
applies_to: soc=official_product_support_table; cann=8.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

`aclrtMalloc` 输出 device 地址并按 device bytes 分配，与 `aclrtFree` 配对。

依据：`S-MALLOC-800`（官方文档归纳，未建立本条实测对应）。

### Host 分配的容量与释放配对

```yaml
applies_to: soc=official_product_support_table; cann=8.5.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

Host 分配长度按 bytes 解释，分配结果与 `aclrtFreeHost` 配对。

依据：`S-MALLOC-HOST-850`（官方文档归纳，未建立本条实测对应）。

### Host 内存释放前需要完成证据

```yaml
applies_to: soc=official_product_support_table; cann=8.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

只释放 `aclrtMallocHost` 分配的内存，接口不隐式同步；释放前必须已有完成证据。

依据：`S-FREE-HOST-800`（官方文档归纳，未建立本条实测对应）。

### Device 内存释放不替代同步

```yaml
applies_to: soc=official_product_support_table; cann=8.5.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

释放适用的 device 分配；调用不替代设备或 Stream 同步。

依据：`S-FREE-850`（官方文档归纳，未建立本条实测对应）。

### 同步边界必须对应任务生产方

```yaml
applies_to: soc=official_product_support_table; cann=8.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

按任务生产 Stream 证明同步关系，不能把不同边界互换。

依据：`S-SYNC-GUIDE-800`（官方文档归纳，未建立本条实测对应）。

### 观察或释放前等待核函数执行完成

```yaml
applies_to: soc=official_product_support_table; cann=8.5.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

观察结果或释放相关资源前建立相应完成边界。

依据：`S-EXEC-850`（官方文档归纳，未建立本条实测对应）。

## 最小检查顺序

1. 列出资源创建点、成功输出地址、所有使用点和所有释放点。
2. 先确认所有排队中的使用、异步拷贝和回调均已越过对应完成边界；缺少证据时记录缺口，不认定可以安全释放。
3. 将每个分配 API 与目标头文件及版本文档中的释放族配对。
4. 检查失败分支和收尾路径是否有重复释放、提前释放或遗漏所有权。
5. 只修复第一个被证据确认的配对或顺序错误，保留所有验证分支。

消费者完成是检查清理路径的前提，不能等排除了重复释放才检查。

## 待验证假设与常见误判

- CANN 9.0.0 的 `aclrtFreeHost` 支持 `aclrtMallocHost` 和 `aclrtMallocHostWithCfg` 分配；不要把旧文档列出的 allocator 当成新版本的完整集合。
- `aclrtFree` 不替代设备或 Stream 同步；需另外证明所有消费者完成。
- DVPP 等本轮未核实的分配族必须按目标版本重新查证，不在本页推断释放配对。
- 外部对象的销毁责任、别名指针和失败路径不清楚时，标记 `UNRESOLVED` 并指出缺失 owner 证据。

## 验证与退出

保留原来的失败条件、返回语义和验证器。以同一命令确认修复后资源仍有效至最后一次使用，且清理只执行一次。若需改动受保护验证面，停止并报告原因。

## 官方版本定位

本页重点核对 `aclrtFree`、`aclrtFreeHost`、对应分配族和完成边界。引用分配/释放文档时须说明：释放前仍要取得所有消费者完成的证据，并核对目标版本的释放与同步语义；文档链接本身不是完成证明。Kernel 包的定位与加载证据见 [Kernel 包来源追溯](kernel-package-provenance.md)。

| CANN 版本 | Device 释放 | Host 释放 | 差异处理 |
| --- | --- | --- | --- |
| 8.0.0 | [`aclrtFree`](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0100.html) | [`aclrtFreeHost`](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0102.html) | 只按页面列出的 allocator 配对；释放不冒充同步。 |
| 8.1.RC1.alpha001 | [`aclrtFree`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha001/apiref/appdevgapi/aclcppdevg_03_0100.html) | [`aclrtFreeHost`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha001/apiref/appdevgapi/aclcppdevg_03_0102.html) | 核对本版分配族和产品支持表。 |
| 8.2.RC1.alpha001 | [`aclrtFree`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/appdevgapi/aclcppdevg_03_0100.html) | [`aclrtFreeHost`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/appdevgapi/aclcppdevg_03_0102.html) | 不从 8.1 或 8.3 外推 allocator 列表。 |
| 8.3.RC1.alpha001 | [`aclrtFree`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/83RC1alpha001/API/appdevgapi/aclcppdevg_03_0100.html) | [`aclrtFreeHost`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/83RC1alpha001/API/appdevgapi/aclcppdevg_03_0102.html) | 核对释放是否隐式等待及调用后指针约束。 |
| 8.5.0 | [`aclrtFree`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/appdevgapi/aclcppdevg_03_0100.html) | [`aclrtFreeHost`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/appdevgapi/aclcppdevg_03_0102.html) | 算子异步执行后先建立对应 Stream/Event 完成边界。 |
| 9.0.0 | [`aclrtFree`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/runtimeapi/aclcppdevg_03_0100.html) | [`aclrtFreeHost`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/runtimeapi/aclcppdevg_03_0102.html)、[`aclrtMallocHost`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/runtimeapi/aclcppdevg_03_0101.html) | 区分普通 `FreeHost` 与带 device 同步的变体；不把两者语义混合。 |
| 9.1.0 | [`aclrtFree`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/910/API/runtimeapi/aclcppdevg_03_0100.html) | [`aclrtFreeHost`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/910/API/runtimeapi/aclcppdevg_03_0102.html) | `aclrtFreeHost` 页面明确立即释放且不隐式同步，并列出 `MallocHostWithCfg` 配对。 |
| 9.2.0-beta.1 | [`aclrtFree`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta1/API/runtimeapi/aclcppdevg_03_0100.html) | [`aclrtFreeHost`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta1/API/runtimeapi/aclcppdevg_03_0102.html) | beta.1 单独核对。 |
| 9.2.0-beta.2 | [`aclrtFree`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta2/API/runtimeapi/aclcppdevg_03_0100.html) | [`aclrtFreeHost`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta2/API/runtimeapi/aclcppdevg_03_0102.html) | 当前最新可核对公开 beta 页面。 |
| 9.3.0 | 未找到对应的官方公开页面 | 未找到对应的官方公开页面 | 当前 950 环境需以安装头文件、包路径和实际行为为准。 |

## 来源

### CANN 9.0.0

- `S-FREE-HOST-900` — [aclrtFreeHost](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0102.html)、[aclrtMallocHost](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0101.html)，CANN 商用版 9.0.0

  - API：`aclrtFreeHost`、`aclrtMallocHost`；文档版本：9.0.0，产品范围按页面支持表。
  - 引用内容：普通 Host 释放与带同步释放不能混用语义。

### 其他版本的文档依据

- `S-MALLOC-800` — [aclrtMalloc，CANN 商用版 8.0.0](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0095.html)

  - API：`aclrtMalloc`、`aclrtFree`。
  - 文档版本：CANN 商用版 8.0.0。
  - 引用内容：Device 分配的输出、单位与释放配对。

- `S-MALLOC-HOST-850` — [aclrtMallocHost，CANN 商用版 8.5.0](https://www.hiascend.com/document/detail/en/canncommercial/850/API/appdevgapi/aclcppdevg_03_0101.html)

  - API：`aclrtMallocHost`、`aclrtFreeHost`。
  - 文档版本：CANN 商用版 8.5.0。
  - 引用内容：Host 分配的容量与释放配对。

- `S-FREE-HOST-800` — [aclrtFreeHost，CANN 商用版 8.0.0](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0102.html)

  - API：`aclrtFreeHost`。
  - 文档版本：CANN 商用版 8.0.0。
  - 引用内容：Host 内存释放前需要完成证据。

- `S-FREE-850` — [aclrtFree，CANN 商用版 8.5.0](https://www.hiascend.com/doc_center/source/en/canncommercial/850/API/appdevgapi/aclcppdevg_03_0100.html)

  - API：`aclrtFree` 及文档列出的 device 分配族。
  - 文档版本：CANN 商用版 8.5.0。
  - 引用内容：Device 内存释放不替代同步。

- `S-SYNC-GUIDE-800` — [Synchronization，CANN 商用版 8.0.0](https://www.hiascend.com/document/detail/en/canncommercial/800/appdevg/aclcppdevg/aclcppdevg_000013.html)

  - API/边界：Stream 内、Stream 间和 Event 同步。
  - 文档版本：CANN 商用版 8.0.0。
  - 引用内容：同步边界必须对应任务生产方。

- `S-EXEC-850` — [Operator Execution，CANN 商用版 8.5.0](https://www.hiascend.com/document/detail/en/canncommercial/850/opdevg/Ascendcopdevg/atlas_ascendc_10_00043.html)

  - API：Ascend C 核函数异步调用与 `aclrtSynchronizeStream`。
  - 文档版本：CANN 商用版 8.5.0。
  - 引用内容：观察或释放前等待核函数执行完成。
