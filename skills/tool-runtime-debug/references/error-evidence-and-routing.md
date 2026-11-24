# 错误证据与分流

## 何时阅读

当首次失败返回非零值、`aclGetRecentErrMsg()` 含有补充信息，或现有证据不足以判断应继续 Host Runtime 排查还是转向其他领域时阅读本页。

## 排查原则

返回码用于缩小检查范围，不能单独证明根本原因。先固定最早失败调用、原始返回值、同线程错误文本和对应日志位置，再结合参数、对象关系与时间线决定下一步。同步接口暴露的设备错误必须追溯到更早的异步任务。

## 先保存不会重现的证据

1. 记录第一个非零返回值、API 名、线程、device、Context、Stream、输入摘要和复现命令。
2. 在同一线程、调用其他 ACL 接口之前读取并保存 `aclGetRecentErrMsg()`；后续调用可能改变可取得的最近错误信息。
3. 只截取与本次进程、线程、device、stream 和时间窗匹配的 plog，并保留原始时间戳。
4. 若错误在同步点被观察到，将 `first_failure` 追溯到队列中最早异常任务，同时保留同步点作为 Host 观察位置。
5. 错误文本、错误码和日志互相矛盾时，不任选其一作为结论；记录冲突并返回 `UNRESOLVED`。

## 返回码只决定检查方向

| 返回码 | 可以确认的范围 | 下一步 |
| --- | --- | --- |
| `0` | 本次 API 按接口约定返回成功 | 继续核对输出参数、完成边界和实际数据，不把成功返回等同于任务完成。 |
| `161001` | 参数中存在非法空指针 | 对照目标版本的具体算子页检查所有必填参数，包括描述对象、executor 和 workspace。 |
| `161002` | 参数不满足接口约定 | 核对 dtype、shape、format、属性范围及输入间推导关系。 |
| `361001` | ACLNN 内部调用 Runtime 失败 | 保存最近错误文本和 plog 中的底层 Runtime 错误，再按最早设备或 Host 失败分流。 |
| `561001` | 输出 shape 推导失败 | 保留输入与输出描述；证据进入形状推导后停止 Host Runtime 修改。 |
| `561002` | Tiling 失败 | 保留输入、属性和失败文本，转向 Tiling。 |
| `561003` | Kernel 查找失败 | 先查实际加载的算子包和配置；SEL、TilingKey 或编译产物问题转向 Compile/Tiling。 |
| `561107` | 未检测到算子包路径 | 核对当前进程环境及其来源，不在诊断中臆造或覆盖环境变量。 |
| `561112` | 算子二进制 Kernel 库未加载 | 查实际包路径、vendor 配置和已安装产物，不直接重装或删除。 |
| 其他 `561xxx` | ACLNN 内部 executor、属性、配置、JSON 或包处理异常 | 以目标版本公共返回码页和完整错误文本为准，不按数字范围猜具体含义。 |
| `507035` | 设备侧向量核异常被 Host 观察到 | 先核对 Host 容量、地址和提交关系；Host 边界成立后转向 AI Core。 |
| `507046` | Stream 在限定时间内未完成 | 查阻塞任务和等待关系，不按 `507035` 的内存方向修改。 |

## 版本经验

以下标签仅约束各自条目，不代表整篇或整个 Skill 的适用范围。`official_product_support_table` 严格以条目所引官方页面为准。

### 最近错误文本必须紧邻失败调用采集

```yaml
applies_to: soc=official_product_support_table; cann=8.0.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

`aclGetRecentErrMsg()` 返回当前线程最近错误的描述；应在失败调用后立即复制所需文本，不能把较晚调用取得的内容追认成原失败证据。

依据：`S-RECENT-800`（官方文档归纳，未建立本条实测对应）。

### ACLNN 分类码与 Runtime 透传码分开处理

```yaml
applies_to: soc=official_product_support_table; cann=9.1.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

`161xxx`、`361xxx` 和 `561xxx` 用于定位 ACLNN 参数、Runtime 调用或内部处理方向；Runtime 透传错误保留其原值。不能用外层 `361001` 替代底层 Runtime 错误，也不能由某个 `561xxx` 直接推导代码修改。

依据：`S-ACLNN-CODES-910`（官方文档归纳，未建立本条实测对应）。

## 最小检查顺序

1. 固定最早非零返回、同线程错误文本和对应日志位置。
2. 在目标版本官方页面或安装头文件中确认码值与符号，避免跨版本套用名称。
3. 判断错误发生在参数校验、Host Runtime、环境与算子包、Tiling、Compile、AI Core、Precision 还是多卡通信。
4. 只有 Host Runtime 证据充分时，选择其余六篇专题之一继续；否则保留现场并停止跨领域修改。
5. 新证据改变最早失败点时，重新分流，不保留已经被推翻的根本原因叙述。

## 常见误判

- `0` 只说明该调用返回成功，不证明异步任务完成、输出有效或后续消费者正确。
- 同步 API 返回设备错误，不等于同步 API 产生了设备错误。
- 错误码名称相同，不代表不同 CANN 版本的产品支持范围和补充限制完全相同。
- 搜索到相同数字但没有目标版本、API 和完整错误文本时，不据此修改源码。
- 日志解析器输出是索引结果，不是根本原因证明；必须能回到原始日志行。

## 验证与退出

本页只建立证据和选择方向，不直接证明修复成功。找不到目标版本定义、错误文本已被覆盖、日志时间窗无法对应，或多个方向仍同样可能时，返回 `UNRESOLVED` 并列出缺失证据。

## 官方版本定位

优先读取目标版本的 ACLNN 公共返回码和 `aclGetRecentErrMsg()` 页面。页面发生跳转时核对跳转后的标题与版本，不因 HTTP 成功就认定页面内容匹配。

| CANN 版本 | 官方文档 | 本页需核对的差异 |
| --- | --- | --- |
| 8.0.0 | [`aclGetRecentErrMsg`](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclpythondevg_01_0009.html) | 核对线程范围、返回文本生命期和调用时机。 |
| 8.1.RC1.alpha001 | [ACLNN 公共返回码](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha001/apiref/aolapi/operatorlist_00423.html) | 按 alpha001 页面确认分类码，不向 8.2 外推。 |
| 8.2.RC1.alpha001 | [`aclGetRecentErrMsg`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/appdevgapi/aclcppdevg_03_0010.html) | 公开公共返回码页未在本次定位到；具体 ACLNN 码须查对应算子页和安装头文件。 |
| 8.3.RC1.alpha001 | [`aclGetRecentErrMsg`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/83RC1alpha001/API/appdevgapi/aclcppdevg_03_0010.html) | 分别核对 ACL 与 ACLNN 层错误，不合并解释。 |
| 8.5.0 | [ACLNN 公共返回码](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/aolapi/operatorlist_00423.html) | 使用 8.5.0 的分类、名称和产品支持表。 |
| 9.0.0 | [ACLNN 公共返回码](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/aolapi/operatorlist_00423.html) | 当前 910 环境应以本版页面和安装头文件共同确认。 |
| 9.1.0 | [ACLNN 公共返回码](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/910/API/aolapi/operatorlist_00423.html)、[`aclError`](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/910/API/ascendgraphapi/aclcppge_00_0010.html) | 页面包含 Ascend 950 系列支持信息；不把系列名替换为未明示的子型号。 |
| 9.2.0-beta.1 | [ACLNN 公共返回码](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta1/API/aolapi/operatorlist_00423.html) | beta.1 独立核对。 |
| 9.2.0-beta.2 | [ACLNN 公共返回码](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta2/API/aolapi/operatorlist_00423.html) | beta.2 独立核对，不写成正式版。 |
| 9.3.0 | 未找到对应的官方公开页面 | 当前 950 环境必须以安装头文件、原始错误文本和实际日志为准。 |

## 来源

- `S-RECENT-800` — [aclGetRecentErrMsg，CANN 商用版 8.0.0](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclpythondevg_01_0009.html)

  - API：`aclGetRecentErrMsg`。
  - 文档版本：CANN 商用版 8.0.0，产品范围按页面支持表。
  - 引用内容：最近错误文本的线程范围、取得时机与生命期。

- `S-ACLNN-CODES-910` — [ACLNN 公共返回码，CANN 社区版 9.1.0](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/910/API/aolapi/operatorlist_00423.html)

  - API/枚举：ACLNN 公共返回码与 Runtime 透传错误。
  - 文档版本：CANN 社区版 9.1.0，产品范围按页面支持表。
  - 引用内容：参数、Runtime 和内部异常的分类边界。
