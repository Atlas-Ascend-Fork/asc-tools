# comm-operator-anomaly（通信算子异常）

## 典型现象

**算子/HCCL 层 API 契约违反**：单个 rank 调用通信算子时即违反 API 契约（绝对违反，不依赖 rank 间对比），触发崩溃、segfault 或 HCCL_E_PARA。

可观测信号：
- 返回码：`HCCL_E_PARA`(1) 或 segfault / 非 0 退出
- driver 日志：`comm operator anomaly detected: HcclAllReduce count-OOB failed`
- aclnn 错误码只指示失败阶段或类别：`161001` 为必需参数空指针，`561002` 为 tiling 错误，`561003` 为 kernel 查找失败，`561000` 为通用内部错误。结合该次调用的详细错误文本定位，不能仅凭号码断定算子未注册。
- plog device：`TS_ERROR`、`aicore error`、通信算子 kernel abort
- 严重度：critical（崩溃型）

**常见根因**：
| 类型 | 表现 |
|------|------|
| count OOB / **值域违反** | count 远超 sendBuf/recvBuf 实际分配大小（如 count=elem*1024 但 buffer 仅 elem*2B）→ HCCL 越界访问；或参数值为溢出回绕/近类型极值的非法值（单 rank 视角即"不可能合法"），被入口参数检查拒绝（`HCCL_E_PARA`）即时确定性失败——**单 rank 条件注入形态下其余 rank 正常参与并等待超时，不改判 single-rank-block（退化值限零/空/nullptr）/collective-param-mismatch（各 rank 合法互异或集合调用点标量 root/srcRank 越界）**；值常源自算子链路上游计算派生（换算溢出/陈旧规模常量越界），须回溯参数派生源 |
| **寻址域违反（P2P 目标参数）** | HcclSend/HcclRecv 的 dstRank/srcRank 越界指向不存在对端——单侧调用的算子契约参数（无跨 rank 一致性维度），入口拒绝即时失败；注意：错配目标若位于**表驱动映射构造**（批次 item 表/对端基）条目 → [rank-group-mismatch](rank-group-mismatch.md)（映射表错配型，见 INDEX 值类参数核对次序第 3 条载体分派） |
| 必需参数为空（161001） | 输入、输出或属性缺少有效对象；先定位日志指出的实参。只有独立日志明确指向加载/注册时，才查算子包与 SoC 配置；执行器创建失败本身不足以推断包缺失 |
| kernel 查找失败（561003） | 核对实际加载的算子包、目标芯片与二进制；不能用参数错误冒充包缺失 |
| kernel 空壳（须有二进制/运行证据） | op_kernel 仅 REGISTER_TILING_DEFAULT 无有效 AscendC API（缺 DataCopy/Compute）→ 空壳 device 二进制 |
| tiling/SetBlockDim 异常 | tiling 函数未正确设置维度/参数 |
| 输出缓冲区不足 | 集合类算子输出缓冲区分配违反算子语义约束（如 AllGather 的 recvBuf 须容纳 sendCount×rankSize，漏乘 rankSize → 越界写/输出截断） |
| 算子语义/块粒度约束违反 | 分块类参数违反算子自身语义约束（如 AlltoAll 各目标分块之和须等于张量第 0 维——**单 rank 视角即可判定违反**，无需 rank 间对比） |

**与"rank 间参数分歧及集合寻址错配"的分界**：调用参数违反**单侧算子或缓冲契约**（缓冲区大小关系、枚举/类型合法域、分块之和与全局输入张量第 0 维约束，以及 P2P 点对点调用标量 `dstRank`/`srcRank` 越界——单 rank 视角即可判定）→ 本类；各 rank 取值各自在合法域内但组内**互不相同**，或**集合调用点标量寻址实参（`root`/`srcRank`）越界或互异**（依 B1 载体分派，包含入口返回 `HCCL_E_PARA` 或等待超时）→ [collective-param-mismatch](collective-param-mismatch.md)。分块粒度类参数两步判：① 先按单 rank 视角核对该 rank 的分块约束——**各目标分块之和须等于全局输入张量第 0 维（各 rank 一致的业务张量元素总数，如 elem_count；不是各 rank 自行按分块截断分配的 sendBuf 大小）**、缓冲区须容纳分块×rankSize——任一 rank 单侧违反（分块之和 < 全局张量维度 = 该 rank 只截取了张量的一部分参与交换）→ 本类；② 各 rank 均自洽（分块之和均等于全局张量维度）、仅块粒度互异 → collective-param-mismatch。

## 调用工具

1. **[plog](../tools/plog.md)**：grep `count|OOB|aclnn|aicore error|TS_ERROR`，定位算子调用失败点
2. **[msaicerr](../tools/msaicerr.md)**：`-e -dev <n>` 在授权空闲设备上运行内置 golden 算子补充环境观测；`-p <report>` 解析已采集的 AI Core error 报告
3. **[msnpureport](../tools/msnpureport.md)**：取 bbox 确认是否 device 内存访问异常
4. **[hccl-test](../tools/hccl-test.md)**：在所测 collective、数据规模、设备与窗口下基准验证纯通信链路行为

## 决策逻辑

详见 [decision-trees.md](../method/decision-trees.md) 树 C。核心判定：

1. 进程崩溃/segfault？
   - 是 → 检查 count 是否远超 buffer → count OOB → 本类
2. aclnn 调用失败？
   - 先区分 GetWorkspaceSize、提交执行、同步等待。读取该次 aclGetRecentErrMsg 或对应 plog，核对最早失败 rank。
   - `161001` → 查必需输入/输出/属性的有效描述符；`561002` → 查 tiling 详细错误，若点名入参约束则核对 shape/属性/平台组合，否则查具体内部失败；`561003` → 查 kernel 包、路径和 SoC。
   - `561000` → 根据详细错误收窄原因，不预设空壳 kernel。原生链路可读[阶段与契约核查](../method/native-fused-operators.md)。
3. msaicerr `-e` 实测结果如何解读？
   - 返回 `MS_AICERR_HARDWARE_ERR` → 内置样例在所测设备与窗口下失败；结合具体错误码、bbox/设备日志及环境版本确认原因，再区分硬件、驱动环境或算子原因
   - 返回 `MS_AICERR_NONE_ERROR` → 仅说明内置样例在所测设备与时间窗口内通过；仍结合原异常报错路径、源码与同步证据核对算子实现或 API 契约 → 本类
4. 值类参数出现"非法/被入口拒绝"表象？→ 按 [INDEX.md](../INDEX.md) B1「值类参数核对次序」分型：零/空/nullptr 退化值 → single-rank-block（调用退化型）；**溢出/超界非法值（值域违反）→ 本类**；集合调用点标量 root/srcRank 越界或互异、或各 rank 取值合法互异 → collective-param-mismatch；错配目标位于表驱动映射构造（批次 item 表/对端基）→ rank-group-mismatch（映射表错配型）；P2P 调用点标量目标越界 → 本类（寻址域违反）

**排除近似类型**：
- 与 [collective-param-mismatch](collective-param-mismatch.md)：本类覆盖单侧违反算子 API/缓冲契约（count OOB、值域违反、分块约束违反）及 **P2P 调用点标量 `dstRank`/`srcRank` 寻址域违反**；collective-param-mismatch 覆盖各 rank 合法取值相对不一致及 **集合调用点标量 `root`/`srcRank` 越界或互异**（依 B1 载体分派，即使入口报 `HCCL_E_PARA` 仍归后者）
- 与 [single-rank-block](single-rank-block.md)（调用退化型）：单 rank 条件注入的非法值，**零/空/nullptr 退化值**（无数据可通信的参与退化）→ single-rank-block；**溢出/超界大值**（以非法载荷调用、参与意图存在）→ 本类（值域违反）——两者调用结局可同构（入口拒绝 + 对端等待超时），须按值语义类别分型，勿按结局
- 与 [rank-group-mismatch](rank-group-mismatch.md)：P2P/批次错配目标位于**调用点标量实参** → 本类（寻址域违反）；位于**表驱动参与者映射构造**（item 表/对端基/peer 映射表，陈旧拓扑残留、跨 rank 视角无法配对）→ rank-group-mismatch（映射表错配型）
- 与硬件线索：msaicerr `-e` 返回 `MS_AICERR_NONE_ERROR` 仅覆盖内置样例及当前采样窗口；需配合原运行阶段的设备日志（plog/bbox）与源码契约证据共同定位根因

## 定位方法

- count OOB：检查 driver 中 `aclrtMalloc` 分配大小 vs HcclAllReduce 的 count 参数（buf 须至少容纳 count*sizeof(datatype) 字节）
- aclnn 参数错误：检查详细日志指向的张量/属性、描述符与数据地址；输出不被业务读取，不代表该版本接口允许省略输出对象。
- aclnn tiling 错误：记录各 rank 本地 shape 与全局属性，按安装版本的约束判断是否合法；不是所有参数都应在 rank 间相等。
- 仅在错误指向加载/注册时，检查实际库与 kernel 路径、SoC 配置和产物；自定义 kernel 的空壳检查不能套到原生封装器上。

## 修复方向

→ [fix/INDEX.md#comm-operator-anomaly](../fix/INDEX.md#comm-operator-anomaly)
- count OOB：count 与 malloc 大小匹配（`count = elem_count`，buffer 同步分配 `elem_count*sizeof(dtype)`）
- 值域违反：在参数派生源（表常量/换算式）恢复契约合法值——修复语义 = 单侧契约值恢复（非恢复参与、非跨 rank 对齐）
- 寻址域违反（P2P 目标参数）：dstRank/srcRank 指向当前通信域内真实存在的对端
- 算子未注册：安装 custom_opp 包 / 修正 SoC 配置一致性
- kernel 空壳：补齐有效 Ascend C API 调用（DataCopy/Compute/Pipe）
- `code-modification`（在获授权修改范围时于默认 `core` 或 `full` 模式执行最小修改并验证；只读诊断未授权写码时输出 Patch 草案）

## 交叉引用

- 易混淆：[collective-param-mismatch](collective-param-mismatch.md)（rank 间合法参数不一致及集合调用标量 root/srcRank 越界或互异）
- 某 rank 先失败、其他 rank 仍等待时，分别记录首错与传播后果。只有公开业务契约明确允许的替代路径才能采用；不能吞掉算子错误、继续消费未生成的输出，或用主机结果替代要求的设备数据面。
