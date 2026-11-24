# Kernel 包来源追溯

## 何时阅读

当出现 `561003`、`561107`、`561112`，修改 Kernel 后输出不变，或怀疑当前进程加载了错误版本、错误 vendor 或旧算子包时阅读本页。

## 排查原则

构建成功、安装脚本成功和运行时实际命中是三件事。结论必须同时给出当前进程环境、候选包、实际配置、构建产物与安装产物的对应证据；不能从源码目录、构建目录或文件时间单独推断运行时加载来源。

## 建立可审计的来源记录

对一次复现至少记录以下内容：

| 证据 | 必须记录的值 | 不能据此单独推导 |
| --- | --- | --- |
| 运行环境 | CANN 版本、SoC、`ASCEND_HOME_PATH`、`ASCEND_OPP_PATH`、`ASCEND_CUSTOM_OPP_PATH` 的实际值 | 环境变量存在不证明路径有效或已命中目标包。 |
| 包身份 | vendor、算子名、包目录、配置文件和候选动态库 | 找到文件不证明当前进程已加载。 |
| 构建产物 | 路径、大小、修改时间、内容哈希、目标 SoC | 修改时间更新不证明内容变化。 |
| 安装产物 | 路径、大小、修改时间、内容哈希、配置条目 | 哈希相同不证明 SEL、TilingKey、dtype 或 `opParaSize` 正确。 |
| 运行证据 | 原始错误码、错误文本、plog 中命中或查找失败的位置 | 单条日志不证明完整搜索顺序。 |

只读检查优先。未经授权不得删除 vendor 目录、清理全局缓存、覆盖环境变量、重装算子包或修改系统级 `config.ini`。

## 根据症状缩小范围

### `561003`：Kernel 查找失败

先确认当前进程看到的算子包根目录、vendor 配置、算子名、dtype/format 和目标 SoC。只有日志或配置证据明确落在 SEL、TilingKey、Kernel 变体或编译产物时，才转向 Compile/Tiling；Host Runtime 不猜测应生成哪个变体。

### `561107`：算子包路径缺失

记录进程实际环境以及环境初始化脚本来源。不要为了消除错误临时拼接一个未经核对的路径；先确认目标 CANN 安装、基础 OPP 与自定义 OPP 的职责。

### `561112`：二进制 Kernel 库未加载

区分“文件不存在”“配置未指向该文件”“依赖无法解析”和“进程命中了另一份包”。保留加载失败文本和候选路径，不能把重新安装作为第一步。

### 修改后输出不变

依次比较源码对应的构建产物、部署产物和运行时命中证据。内容哈希可证明两个文件是否相同，但不能证明哪个文件被进程加载；若没有实际命中证据，结论保持 `UNRESOLVED`。

## 版本经验

以下标签仅约束各自条目，不代表整篇或整个 Skill 的适用范围。`official_product_support_table` 严格以条目所引官方页面为准。

### Kernel 查找与算子包路径错误应分开取证

```yaml
applies_to: soc=official_product_support_table; cann=9.1.0; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

`561003`、`561107` 和 `561112` 分别指向 Kernel 查找、OPP 路径检测和二进制 Kernel 包加载方向。三者可以共享环境证据，但不能互换错误含义或采用同一修复结论。

依据：`S-KERNEL-CODES-910`（官方文档归纳，未建立本条实测对应）。

### 调用自定义 ACLNN 接口前须完成对应算子包部署

```yaml
applies_to: soc=official_product_support_table; cann=9.2.0-beta.2; bisheng=not_applicable; op_class=all
verified_on: soc=not_verified; cann=not_verified; bisheng=not_applicable; op_class=not_verified
```

自定义 ACLNN 接口及其实现随自定义算子包交付，调用前需要完成对应包部署；运行时排查应核对当前版本页面给出的 vendor 与库路径，而不是从其他版本复制目录结构。

依据：`S-OP-PACKAGE-92B2`（官方文档归纳，未建立本条实测对应）。

## 最小检查顺序

1. 固定原始错误码、错误文本、复现命令、CANN 版本和目标 SoC。
2. 记录当前进程的 CANN/OPP 环境，不执行会改变环境的初始化或安装动作。
3. 从目标版本官方文档和实际配置得到 vendor、包根目录与库位置；列出所有候选，但不假定搜索顺序。
4. 比较构建产物与安装产物的内容哈希，并从 plog 或加载信息确认实际命中对象。
5. 证据只表明部署不一致时，提出最小部署修正；证据进入 SEL、TilingKey、TilingData、Kernel 编译或设备执行时停止 Host Runtime 修改并分流。

## 常见误判

- `ASCEND_OPP_PATH` 已设置，不等于当前进程能访问目标包，也不等于自定义包应覆盖基础 OPP。
- 只找到一份同名文件，不等于动态加载器或算子框架实际命中了它。
- 修改时间相同或不同都不能代替内容哈希；内容哈希相同也不能代替实际加载证据。
- 重新编译后输出不变，不能直接归因于缓存；也可能是部署产物未更新、命中另一份包或输入没有覆盖修改分支。
- `561003` 不自动等于 SEL/TilingKey 错误；必须先排除路径、包和配置证据。

## 验证与退出

验证时使用原复现命令，并同时确认错误码、实际加载对象和完整输出。只修正第一处有证据的来源不一致；未取得加载命中证据时，不宣称包冲突已确认。涉及删除、清缓存、重装或系统配置修改时，先停止并说明拟改对象及影响。

## 官方版本定位

错误码定义与算子包部署说明需要配套阅读。引用这些文档时，必须同时说明：文档只能解释接口和部署规则，实际加载路径仍须取自当前安装环境、配置和加载日志，不能用文档示例路径代替加载证据。旧版本公开站点未必提供同名的工程化部署页面；找不到时应查该版本的算子开发指南和本机安装包，不能套用新版目录。

| CANN 版本 | 错误码或包部署文档 | 本页需核对的差异 |
| --- | --- | --- |
| 8.0.0 | [`aclGetRecentErrMsg`](https://www.hiascend.com/document/detail/en/canncommercial/800/apiref/appdevgapi/aclpythondevg_01_0009.html) | 未在本次定位到同结构的公开包部署页；以 8.0.0 安装路径和算子开发指南为准。 |
| 8.1.RC1.alpha001 | [ACLNN 公共返回码](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/81RC1alpha001/apiref/aolapi/operatorlist_00423.html) | 核对 alpha001 的错误定义与实际 OPP 布局。 |
| 8.2.RC1.alpha001 | [`aclGetRecentErrMsg`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/82RC1alpha001/API/appdevgapi/aclcppdevg_03_0010.html) | 未定位到可直接复用的公共包部署页；不得从 8.3 或 8.5 推断搜索路径。 |
| 8.3.RC1.alpha001 | [`aclGetRecentErrMsg`](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/83RC1alpha001/API/appdevgapi/aclcppdevg_03_0010.html) | 以本版安装包配置和实际加载日志为准。 |
| 8.5.0 | [ACLNN 公共返回码](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/850/API/aolapi/operatorlist_00423.html) | 核对 8.5.0 的 vendor 配置、包目录和产品支持范围。 |
| 9.0.0 | [ACLNN 公共返回码](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/900/API/aolapi/operatorlist_00423.html) | 当前 910 环境须同时记录本机安装路径和实际加载证据。 |
| 9.1.0 | [ACLNN 公共返回码](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/910/API/aolapi/operatorlist_00423.html) | 页面用于区分 Kernel 查找、OPP 路径和二进制包加载方向。 |
| 9.2.0-beta.1 | [ACLNN 公共返回码](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta1/API/aolapi/operatorlist_00423.html) | beta.1 的包布局单独核对，不与 beta.2 合并。 |
| 9.2.0-beta.2 | [单算子 API 调用](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta2/programug/Ascendcopdevg/docs/zh/guide/programming_guide/advanced_programming/aclnn_operator_development/invocation/single_operator_api_call.md)、[ACLNN 公共返回码](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta2/API/aolapi/operatorlist_00423.html) | 可核对自定义算子包部署与 ACLNN 调用的对应关系；仍不代表 9.3.0。 |
| 9.3.0 | 未找到对应的官方公开页面 | 当前 950 环境必须以安装包、配置和实际加载日志为准。 |

## 来源

- `S-KERNEL-CODES-910` — [ACLNN 公共返回码，CANN 社区版 9.1.0](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/910/API/aolapi/operatorlist_00423.html)

  - API/枚举：`561003`、`561107`、`561112` 等 ACLNN 内部异常。
  - 文档版本：CANN 社区版 9.1.0，产品范围按页面支持表。
  - 引用内容：Kernel 查找、OPP 路径和二进制包加载错误的分类边界。

- `S-OP-PACKAGE-92B2` — [单算子 API 调用，CANN 社区版 9.2.0-beta.2](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta2/programug/Ascendcopdevg/docs/zh/guide/programming_guide/advanced_programming/aclnn_operator_development/invocation/single_operator_api_call.md)

  - API/边界：自定义 ACLNN 接口调用与算子包部署。
  - 文档版本：CANN 社区版 9.2.0-beta.2，产品范围按页面支持表。
  - 引用内容：调用前部署对应自定义算子包，并按本版路径定位接口库。
