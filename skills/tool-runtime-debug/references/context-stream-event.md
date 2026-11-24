# 上下文、Stream 与 Event

## 何时阅读

当调用使用了错误的当前 Context、Stream/Event 身份错用，或等待与提交顺序不能解释观察结果时阅读本页。

若失败发生在设备初始化或核数查询，本页只保留具体对象身份与完成关系；初始化顺序和核数选择应按目标版本的 Host Runtime 规范另行核对。

## 排查原则

逐个对象记录 `create -> bind/use -> record/submit -> wait/query -> synchronize -> destroy` 的时间线，并注明每次调用使用的 Context 和 Stream。先确定对象身份与线程间顺序，再应用下列版本经验。

## 同步点报错不等于同步点产生错误

`507035` 可能在 Stream 同步时被 Host 观察到。这只证明异步队列中更早的任务在此暴露，不证明同步 API 本身产生了错误。

- `507035 / ACL_ERROR_RT_VECTOR_CORE_EXCEPTION`：保留生产 Stream、最后一个成功 enqueue 和 plog 中的第一个 device 异常；Host 边界无误时转向 AI Core。
- `507046 / ACL_ERROR_RT_STREAM_SYNC_TIMEOUT`：表示限时等待内 Stream 任务未完成，不按向量核异常的内存对齐方向盲改。
- `107002/107003`：先核对当前 Context 是否存在，以及 Stream 是否属于当前 Context。

只有证据指向错误 Context、错误 Stream、Event 捕获边界或销毁顺序时，才在本页修复。

## 版本经验

以下标签仅约束各自条目，不代表整篇或整个 Skill 的适用范围。`official_product_support_table` 严格以条目所引官方页面为准。

### 当前 Context 与 Event 的提交、等待和查询边界

```yaml
applies_to: soc=official_product_support_table; cann=9.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

当前 Context 以最后设置为准；record 捕获既有提交，wait 约束后续任务；跨线程 record/query 必须有顺序证据。ExWithFlag Event 的首次 record 前 COMPLETE 和重复 record 覆盖语义按该创建方式处理，不能外推到全部 Event。

文档依据：`S-CONTEXT-EVENT-900`。

### Event 超时参数不能自行解释为轮询

```yaml
applies_to: soc=official_product_support_table; cann=9.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

文档明确 -1 为永久等待、正数为毫秒；仍未给 0 定义轮询语义。

文档依据：`S-TIMEOUT-900`。

### 每线程 Context 与默认 Stream 的生命周期

```yaml
applies_to: soc=official_product_support_table; cann=8.5.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

每线程当前 Context、显式设置多 Context，以及默认 Stream 不用 `aclrtDestroyStream` 销毁。

依据：`S-CONTEXT-850`（官方文档归纳，未建立本条实测对应）。

### 显式设置当前 Context

```yaml
applies_to: soc=official_product_support_table; cann=8.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

最近一次设置决定线程当前 Context；跨线程使用须明确顺序。

依据：`S-SET-CONTEXT-800`（官方文档归纳，未建立本条实测对应）。

### Event 记录成功不等于任务完成

```yaml
applies_to: soc=official_product_support_table; cann=8.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

Event 捕获在指定 Stream 上 record 时已经下发的任务；成功只表示 record 请求已下发。

依据：`S-RECORD-EVENT-800`（官方文档归纳，未建立本条实测对应）。

### Stream 等待约束后续任务

```yaml
applies_to: soc=official_product_support_table; cann=8.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

等待 Stream 后续任务在 Event 所捕获任务完成后执行；wait 成功本身只表示请求已下发。

依据：`S-WAIT-EVENT-800`（官方文档归纳，未建立本条实测对应）。

### NOT_READY 不是完成证据

```yaml
applies_to: soc=official_product_support_table; cann=8.1.RC1.alpha001; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

`NOT_READY` 不是完成证据；跨线程 record/query 可能乱序。

依据：`S-QUERY-EVENT-81A1`（官方文档归纳，未建立本条实测对应）。

### 超时参数的已定义取值

```yaml
applies_to: soc=official_product_support_table; cann=8.2.RC1.alpha001; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

页面定义 `-1` 为永久等待、正数为毫秒超时；未定义 0 的通用语义，因此不能据此断言 0 等于轮询。

依据：`S-SYNC-TIMEOUT-82A1`（官方文档归纳，未建立本条实测对应）。

## 最小检查顺序

1. 从最早失败调用开始，逐项记录 Context、device、Stream 和 Event。
2. 对照目标头文件确认创建、设置、记录、等待、查询和销毁的参数。
3. 确认 record 发生在 wait/query 之前，并核对被等待、查询和销毁的是同一个 Event。
4. 确认 wait 依赖的 Event 由预期 Stream 记录，且实际观察位于正确的完成边界之后。
5. 只改第一处由证据确认的身份或顺序错误，然后使用同一命令观察。

## 待验证假设与常见误判

- Event wait 不会替无关 Stream 建立依赖；不同 Stream 之间的同步关系必须由可证明的 Event 或目标 API 明确支持。
- 不要把“timeout 为 0”概括成轮询。所引来源没有为该 API 定义 0，目标版本的 0 应按待验证处理。
- 线程间先后不能从源码中的行序推断；需要实际的线程同步证据。
- 版本、调用线程、创建 Context 或 Event 生产 Stream 不清楚时，不猜顺序，先报告缺少的对象关系。

## 验证与退出

以相同输入检查 record、wait/query 和最终观察的先后；不得用 Host `sleep` 代替同步。若最早失败转为设备 Kernel、Tiling 或多卡通信，停止 Runtime 排查并写中性领域名称。

## 官方版本定位

本页重点核对 `aclrtSetCurrentContext`、`aclrtRecordEvent`、`aclrtQueryEventStatus`、`aclrtStreamWaitEvent` 和带超时的 Event 等待。需要其他接口时，从同版本的父主题进入，不切换版本。

| CANN 版本 | Context 官方页 | Event 官方页 | 差异处理 |
| --- | --- | --- | --- |
| 8.0.0 | [`aclrtSetCurrentContext`](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0061.html) | [`aclrtRecordEvent`](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0083.html)、[`aclrtStreamWaitEvent`](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0091.html) | 使用 8.0.0 定义的当前 Context 和 Event 捕获顺序。 |
| 8.1.RC1.alpha001 | [`aclrtSetCurrentContext`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha001/apiref/appdevgapi/aclcppdevg_03_0061.html) | [`aclrtRecordEvent`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha001/apiref/appdevgapi/aclcppdevg_03_0083.html)、[`aclrtQueryEventStatus`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha001/apiref/appdevgapi/aclcppdevg_03_0086.html) | Query/Record 跨线程可乱序；`NOT_READY` 不是完成证据。 |
| 8.2.RC1.alpha001 | [`aclrtSetCurrentContext`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/appdevgapi/aclcppdevg_03_0061.html) | [`aclrtRecordEvent`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/appdevgapi/aclcppdevg_03_0083.html)、[`aclrtSynchronizeEventWithTimeout`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/appdevgapi/aclcppdevg_03_0089.html) | 带超时 API 的参数边界按本版页面处理。 |
| 8.3.RC1.alpha001 | [`aclrtSetCurrentContext`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/83RC1alpha001/API/appdevgapi/aclcppdevg_03_0061.html) | [`aclrtRecordEvent`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/83RC1alpha001/API/appdevgapi/aclcppdevg_03_0083.html)、[`aclrtQueryEventStatus`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/83RC1alpha001/API/appdevgapi/aclcppdevg_03_0086.html) | 核对产品支持表和 Event 重用约束。 |
| 8.5.0 | [`aclrtSetCurrentContext`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/appdevgapi/aclcppdevg_03_0061.html) | [`aclrtRecordEvent`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/appdevgapi/aclcppdevg_03_0083.html)、[`aclrtQueryEventStatus`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/appdevgapi/aclcppdevg_03_0086.html) | 不用旧版 Event 销毁顺序覆盖本版说明。 |
| 9.0.0 | [`aclrtSetCurrentContext`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/runtimeapi/aclcppdevg_03_0061.html) | [`aclrtRecordEvent`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/runtimeapi/aclcppdevg_03_0083.html)、[`aclrtStreamWaitEvent`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/runtimeapi/aclcppdevg_03_0091.html) | 当前 910 环境对应的官方 API 版本。 |
| 9.1.0 | [`aclrtSetCurrentContext`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/runtimeapi/aclcppdevg_03_0061.html) | [`aclrtRecordEvent`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/runtimeapi/aclcppdevg_03_0083.html)、[`aclrtQueryEventStatus`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/runtimeapi/aclcppdevg_03_0086.html) | 页面公开列出 Ascend 950PR/950DT 支持；不外推到 9.3.0。 |
| 9.2.0-beta.1 | [`aclrtSetCurrentContext`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta1/API/runtimeapi/aclcppdevg_03_0061.html) | [`aclrtRecordEvent`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta1/API/runtimeapi/aclcppdevg_03_0083.html)、[`aclrtQueryEventStatus`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta1/API/runtimeapi/aclcppdevg_03_0086.html) | beta.1 与 beta.2 分开引用。 |
| 9.2.0-beta.2 | [`aclrtSetCurrentContext`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta2/API/runtimeapi/aclcppdevg_03_0061.html) | [`aclrtRecordEvent`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta2/API/runtimeapi/aclcppdevg_03_0083.html)、[`aclrtQueryEventStatus`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta2/API/runtimeapi/aclcppdevg_03_0086.html) | 当前最新可核对公开 beta 页面。 |
| 9.3.0 | 未找到对应的官方公开页面 | 未找到对应的官方公开页面 | 当前 950 环境必须核对安装头文件和运行日志。 |

## 来源

### 引用版本选择

给出链接时同时说明：使用其他 CANN 版本时必须改查对应版本页面，不能把本页默认版本当作用户的实际版本。

- 优先使用用户明确指定或已核实的目标 CANN 版本；同一 API 有多个版本时，只用目标版本的来源条目说明其接口约定。
- 未提供目标版本时，本页默认引用版本表中“当前 910 环境对应”的 **CANN 9.0.0** 来源，Context/Stream 关系使用 `S-CONTEXT-EVENT-900` 中的 `aclrtSetCurrentContext` 和 `aclrtStreamWaitEvent` 链接，并说明这是本页的文档默认版本，尚未核实用户实际安装版本。
- `S-SET-CONTEXT-800`、`S-WAIT-EVENT-800` 等其他版本条目仅在目标版本切换到对应版本时引用；不能用旧版链接替代当前版本的链接。
- 本页没有目标版本的官方链接时，明确说明缺失，要求核对该版本官方文档或安装头文件；不得拼接网址、退回默认版本并声称适用，或从相邻版本推定接口语义。

### CANN 9.0.0

- `S-CONTEXT-EVENT-900` — [aclrtSetCurrentContext](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0061.html)、[aclrtRecordEvent](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0083.html)、[aclrtStreamWaitEvent](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0091.html)、[aclrtQueryEventStatus](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0086.html)，CANN 商用版 9.0.0

  - API：上述 Context/Event 接口；文档版本：9.0.0，产品范围按各页面支持表。
  - 引用内容：当前 Context 与 Event 的提交、等待和查询边界。

- `S-TIMEOUT-900` — [aclrtSynchronizeEventWithTimeout，CANN 商用版 9.0.0](https://www.hiascend.com/document/detail/zh/canncommercial/900/API/runtimeapi/aclcppdevg_03_0089.html)

  - API：`aclrtSynchronizeEventWithTimeout`；文档版本：CANN 商用版 9.0.0。
  - 引用内容：Event 超时参数不能自行解释为轮询。

### 其他版本的文档依据

- `S-CONTEXT-850` — [Concepts - Runtime Management，CANN 商用版 8.5.0](https://www.hiascend.com/document/detail/en/canncommercial/850/appdevg/acldevg/aclcppdevg_000524.html)

  - API：`aclrtCreateContext`、`aclrtSetCurrentContext`、`aclrtCreateStream`；文档版本：CANN 商用版 8.5.0。
  - 引用内容：每线程 Context 与默认 Stream 的生命周期。

- `S-SET-CONTEXT-800` — [aclrtSetCurrentContext，CANN 商用版 8.0.0](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0061.html)

  - API：`aclrtSetCurrentContext`；文档版本：CANN 商用版 8.0.0。
  - 引用内容：显式设置当前 Context。

- `S-RECORD-EVENT-800` — [aclrtRecordEvent，CANN 商用版 8.0.0](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0083.html)

  - API：`aclrtRecordEvent`；文档版本：CANN 商用版 8.0.0。
  - 引用内容：Event 记录成功不等于任务完成。

- `S-WAIT-EVENT-800` — [aclrtStreamWaitEvent，CANN 商用版 8.0.0](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclcppdevg_03_0091.html)

  - API：`aclrtStreamWaitEvent`；文档版本：CANN 商用版 8.0.0。
  - 引用内容：Stream 等待约束后续任务。

- `S-QUERY-EVENT-81A1` — [aclrtQueryEventStatus，CANN 8.1.RC1.alpha001](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha001/apiref/appdevgapi/aclcppdevg_03_0086.html)

  - API：`aclrtQueryEventStatus` 与 `NOT_READY` 状态；文档版本：CANN 社区版 8.1.RC1.alpha001。
  - 引用内容：NOT_READY 不是完成证据。

- `S-SYNC-TIMEOUT-82A1` — [aclrtSynchronizeEventWithTimeout，CANN 8.2.RC1.alpha001](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/appdevgapi/aclcppdevg_03_0089.html)

  - API：`aclrtSynchronizeEventWithTimeout`；文档版本：CANN 社区版 8.2.RC1.alpha001。
  - 引用内容：超时参数的已定义取值。
