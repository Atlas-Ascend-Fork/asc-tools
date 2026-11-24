# 异步可观察性

## 何时阅读

当异步提交成功后仍出现旧数据、回调未按预期触发，或查询状态被当作执行完成证据时阅读本页。

## 排查原则

分别记录任务提交、设备执行、完成边界和 Host 观察。下列规则按具体 API 和内存类型使用，不能把某个异步接口的返回语义推广到所有接口。

## 把 plog 还原成时间线

将 plog 和 Host 观察统一还原为四个时点：`submit -> device start -> device failure/complete -> host observe`。只保留与当前复现进程、线程、device、stream 和时间窗匹配的日志行。

1. 将第一个失败 API 的返回值、`aclGetRecentErrMsg()` 和对应 plog 时间戳放在同一条证据中。
2. 若同步 API 只是报告更早的 device 错误，将 `first_failure` 移回最早的异常任务，但保留同步点作为 Host 观察位置。
3. 仅在已获得日志文件、且解析器的参数与输出行为已经核对时使用自动解析；解析成功不等于根本原因确认。
4. 不修改全局日志配置来代替最小复现；需要开启额外日志时，先说明影响和恢复方式。

## 版本经验

以下标签仅约束各自条目，不代表整篇或整个 Skill 的适用范围。`official_product_support_table` 严格以条目所引官方页面为准。

### Host 内存类型决定异步拷贝的返回边界

```yaml
applies_to: soc=official_product_support_table; cann=9.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

锁页 Host 内存需另等复制完成；非锁页 Host 内存的调用在复制完成后返回。此规则不证明回调接口或其他异步 API 的完成语义；它们仍须按自身来源核对。

文档依据：`S-ASYNC-900`。

### 异步拷贝的锁页与非锁页 Host 内存

```yaml
applies_to: soc=official_product_support_table; cann=8.5.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

页锁定 Host 内存场景的成功返回只代表下发，须等完成；非页锁定 Host 内存另有同步返回语义。

依据：`S-MEMCPY-ASYNC-850`（官方文档归纳，未建立本条实测对应）。

### NOT_READY 不是完成证据

```yaml
applies_to: soc=official_product_support_table; cann=8.1.RC1.alpha001; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

`NOT_READY` 不是完成证据；跨线程 record/query 可能乱序。

依据：`S-QUERY-EVENT-81A1`（官方文档归纳，未建立本条实测对应）。

### 回调下发成功不等于执行完成

```yaml
applies_to: soc=official_product_support_table; cann=8.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

callback 是异步任务，成功下发不等于已经执行。

依据：`S-CALLBACK-800`（官方文档归纳，未建立本条实测对应）。

### 同步边界必须对应任务生产方

```yaml
applies_to: soc=official_product_support_table; cann=8.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

同步关系须与实际任务生产 Stream 相符；不同同步边界不能互换。

依据：`S-SYNC-GUIDE-800`（官方文档归纳，未建立本条实测对应）。

## 最小检查顺序

1. 写出 enqueue、设备执行、完成边界和 Host 读取的顺序。
2. 确认拷贝调用使用的 Host 内存类型，以及目标 API 的返回语义。
3. 核对完成边界的 Event/Stream 是否对应实际生产工作的 Stream。
4. 对回调检查注册结果、关联 Stream、payload 所有者和取消/退出顺序。
5. 只调整最早缺失的完成或生命周期条件，再用原输入复现并检查数据。

## 待验证假设与常见误判

- Host buffer 或 `userData` 至完成前仍有效，是由异步调用可能继续访问指针推出的安全前置；对具体 API 和内存类型仍须验证。
- 非页锁定 Host 内存不能一概描述为调用返回后仍异步访问；以目标 API 与内存类型的具体文档为准。
- `NOT_READY` 不能作为修复失败或完成的判据，也不能触发依赖读取、释放或销毁。
- 不用 `sleep`、重复试跑或无条件全设备同步替代已证实的完成关系。

## 验证与退出

让原复现命令在同一完成边界后观察完整输出。记录实际完成次数和间歇性失败；单次结果只说明本次观察，不证明稳定性。回调仍可能执行或外部 owner 不明时，先报告缺少的生命周期证据。

## 官方版本定位

引用异步拷贝文档时，同时说明返回成功的含义须按目标版本解释，不能自动推出 Host 缓冲区已经可以释放；仍须核对内存类型及对应的完成边界。

本页的核心接口是 `aclrtMemcpyAsync`、`aclrtQueryEventStatus` 和 `aclrtLaunchCallback`。异步复制对锁页/非锁页 Host 内存的返回边界可不同，且 EP、RC 等产品形态存在限制；必须读目标版页面的约束段。

| CANN 版本 | 异步复制 | Event/回调 | 差异处理 |
| --- | --- | --- | --- |
| 8.0.0 | [`aclrtMemcpyAsync`](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0106.html) | [`aclrtQueryEventStatus`](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0086.html)、[`aclrtLaunchCallback`](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0126.html) | 回调下发成功不等于执行完成。 |
| 8.1.RC1.alpha001 | [`aclrtMemcpyAsync`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha001/apiref/appdevgapi/aclcppdevg_03_0106.html) | [`aclrtQueryEventStatus`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha001/apiref/appdevgapi/aclcppdevg_03_0086.html)、[`aclrtLaunchCallback`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha001/apiref/appdevgapi/aclcppdevg_03_0126.html) | 跨线程 record/query 乱序要有额外顺序证据。 |
| 8.2.RC1.alpha001 | [`aclrtMemcpyAsync`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/appdevgapi/aclcppdevg_03_0106.html) | [`aclrtQueryEventStatus`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/appdevgapi/aclcppdevg_03_0086.html)、[`aclrtLaunchCallback`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/appdevgapi/aclcppdevg_03_0126.html) | 核对本版 Host 内存类型和产品形态限制。 |
| 8.3.RC1.alpha001 | [`aclrtMemcpyAsync`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/83RC1alpha001/API/appdevgapi/aclcppdevg_03_0106.html) | [`aclrtQueryEventStatus`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/83RC1alpha001/API/appdevgapi/aclcppdevg_03_0086.html)、[`aclrtLaunchCallback`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/83RC1alpha001/API/appdevgapi/aclcppdevg_03_0126.html) | 不把某一 Host 内存类型的语义外推到其他类型。 |
| 8.5.0 | [`aclrtMemcpyAsync`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/appdevgapi/aclcppdevg_03_0106.html) | [`aclrtQueryEventStatus`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/appdevgapi/aclcppdevg_03_0086.html)、[`aclrtLaunchCallback`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/appdevgapi/aclcppdevg_03_0126.html) | 同版应用指南对异步 H2H 和锁页 Host 内存有额外说明。 |
| 9.0.0 | [`aclrtMemcpyAsync`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/runtimeapi/aclcppdevg_03_0106.html) | [`aclrtQueryEventStatus`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/runtimeapi/aclcppdevg_03_0086.html)、[`aclrtLaunchCallback`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/runtimeapi/aclcppdevg_03_0126.html) | 当前 910 环境的目标页面；锁页 Host 内存需以同步边界证明完成。 |
| 9.1.0 | [`aclrtMemcpyAsync`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/910/API/runtimeapi/aclpythondevg_01_0121.html) | [`aclrtQueryEventStatus`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/runtimeapi/aclcppdevg_03_0086.html)、[`aclrtLaunchCallback`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/runtimeapi/aclcppdevg_03_0126.html) | 页面明确非锁页 Host 内存在复制完成后返回，锁页 Host 内存为真异步，并列出 EP/RC 限制。 |
| 9.2.0-beta.1 | [`aclrtMemcpyAsync`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta1/API/runtimeapi/aclcppdevg_03_0106.html) | [`aclrtQueryEventStatus`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta1/API/runtimeapi/aclcppdevg_03_0086.html) | beta.1 与 beta.2 不合并。 |
| 9.2.0-beta.2 | [`aclrtMemcpyAsync`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta2/API/runtimeapi/aclcppdevg_03_0106.html) | [`aclrtQueryEventStatus`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta2/API/runtimeapi/aclcppdevg_03_0086.html) | 此版还有新的 batch async 接口，遇到该家族时单独查本版页面。 |
| 9.3.0 | 未找到对应的官方公开页面 | 未找到对应的官方公开页面 | 当前 950 环境必须核对安装头文件、plog 和实际完成边界。 |

## 来源

### CANN 9.0.0

- `S-ASYNC-900` — [aclrtMemcpyAsync，CANN 商用版 9.0.0](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0106.html)

  - API：`aclrtMemcpyAsync`；文档版本：9.0.0，产品范围按页面支持表及 EP/RC 形态约束。
  - 引用内容：Host 内存类型决定异步拷贝的返回边界。

### 其他版本的文档依据

- `S-MEMCPY-ASYNC-850` — [memcpy_async，CANN 社区版 8.5.0](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/appdevgapi/aclpythondevg_01_0121.html)

  - API：C 原型 `aclrtMemcpyAsync`。
  - 文档版本：CANN 社区版 8.5.0。
  - 引用内容：异步拷贝的锁页与非锁页 Host 内存。

- `S-QUERY-EVENT-81A1` — [aclrtQueryEventStatus，CANN 8.1.RC1.alpha001](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha001/apiref/appdevgapi/aclcppdevg_03_0086.html)

  - API：`aclrtQueryEventStatus` 与 `NOT_READY`。
  - 文档版本：CANN 社区版 8.1.RC1.alpha001。
  - 引用内容：NOT_READY 不是完成证据。

- `S-CALLBACK-800` — [aclrtLaunchCallback，CANN 商用版 8.0.0](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0126.html)

  - API：`aclrtSubscribeReport`、`aclrtLaunchCallback`、`aclrtProcessReport`、 `aclrtUnSubscribeReport`。
  - 文档版本：CANN 商用版 8.0.0。
  - 引用内容：回调下发成功不等于执行完成。

- `S-SYNC-GUIDE-800` — [Synchronization，CANN 商用版 8.0.0](https://www.hiascend.com/document/detail/en/canncommercial/800/appdevg/aclcppdevg/aclcppdevg_000013.html)

  - API/边界：Stream 内、Stream 间和 Event 同步。
  - 文档版本：CANN 商用版 8.0.0。
  - 引用内容：同步边界必须对应任务生产方。
