# AIV：GM/UB 搬运对齐核查

只在尾部数据缺失、长度敏感或GM/UB区间证据指向搬运时读取；不把所有通信数据错误都归为对齐问题。

- 普通连续DataCopy的count单位为元素；count×sizeof(T)未达32字节对齐时，文档规定实际搬运量向下取整。还需分别核对UB地址的32字节对齐、GM地址的数据类型对齐。
- 可根据目标平台与重载选择DataCopyPad等非对齐设备搬运机制，或其他已确认合法的设备方案，不强制单一修法。DataCopyExtParams.blockLen单位是字节，必须匹配dtype；UB一侧仍需对齐并有足够空间容纳实际数据与内部填充。GMToUB与UBToGM的参数、stride单位和填充方向需分别核对，不能互抄。
- 始终保留精确有效长度和所有源/目标容量边界。不能为满足对齐而盲目增大GM读写范围、覆盖相邻数据或用主机拷贝替代设备路径；验证主体与尾部、每个rank及约定轮次。

狭义 verified_on：Ascend 910B1 / CANN 9.2.0-beta.2，float32、不带mode的DataCopyPad双方向重载（单块、精确字节长度、零stride，GMToUB不启用显式左右padding）已在两组独立数据上通过设备结果比对；同一路径可复用该证据，不因该版在线文档不可读而反复搜索。偏离所测设备、dtype、重载或参数域时再定向核实，不扩展为所有重载可用。

文档依据仍为下列9.2.0-beta.1官方原文（A2支持普通/不带mode的相应原型）；未声称核验了beta.2文档：

- [DataCopy：GM与UB连续搬运](https://www.hiascend.com/doc_center/source/zh/CANNCommunityEdition/920beta1/API/ascendcopapi/docs/zh/api/SIMD-API/基础API/Memory矢量计算/数据搬运/DataCopy_GMAndUB_continuous.md)
- [DataCopyPad：GMToUB](https://www.hiascend.com/doc_center/source/zh/CANNCommunityEdition/920beta1/API/ascendcopapi/docs/zh/api/SIMD-API/基础API/Memory矢量计算/数据搬运/DataCopyPad_GMToUB.md)
- [DataCopyPad：UBToGM](https://www.hiascend.com/doc_center/source/zh/CANNCommunityEdition/920beta1/API/ascendcopapi/docs/zh/api/SIMD-API/基础API/Memory矢量计算/数据搬运/DataCopyPad_UBToGM.md)
