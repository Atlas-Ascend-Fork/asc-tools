# 版本映射与 deprecated 候选

旧名、deprecated标记和迁移记录可以缩小检索范围。它们不能证明当前 build 已移除某API，也不能证明改名或删除保留了原调用语义。`next_version`是相对描述，不是准确移除版本。

| 历史记录 | 当前需要核验 |
|---|---|
| GetLayerNormNDTillingInfo → GetLayerNormNDTilingInfo | 当前命名空间、声明与拼写 |
| Hccl::SetCcTiling → SetCcTilingV2 | 新旧参数表示和单位；GM指针与offset不能仅改名 |
| Hccl::SetReduceDataTypeAbility → host Tiling配置 | 配置责任、执行阶段及原有功能 |
| GetDumpWorkSpacePtr / SetDumpWorkSpacePtr标记deprecated | 当前声明、框架工作区机制与原调用用途 |
| GetLength → 相邻GetSize声明的推测 | 实际对象类型、长度单位与语义，不能当官方等价证明 |
| SetAddrWithOffset → 相邻operator[]声明的推测 | 修改地址状态与返回偏移视图是否等价 |

这些历史条目来自随包引擎 `scripts/engine/harness/knowledge/api_version_map.yaml` 的来源说明，缺少完整的准确build/hash与调用契约；诊断将它们标为待核验候选。先读当前安装的声明和deprecated原文，再判断是否适用于当前对象、参数和目标；只在证据支持时迁移。replacement为空也不授权删除调用。

未命中时按[API存在性排查](hallucinated-api.md)继续；涉及入口条件时读 [头入口与目标](soc-arch-gating.md)。不要从一份教程的命名空间写法推出整个CANN版本的隐式using规则。

## 9.1.0 迁移与归并（28 对）

以下条目依据 v9.0.0 与 v9.1.0 的 SDK 头文件和官方迁移文档整理（逐对登记在 `scripts/engine/harness/knowledge/api_version_map.yaml`，带 `since` 与来源注记）。它们与上文历史条目同一纪律：**待核验候选，非指令**——命中旧名先读当前安装的声明，只在证据支持时迁移；replacement 为空或 inferred 都不授权删除调用。

- **五算子 ops-nn→ops-math 迁移（relocated 5 对）**：Identity/IdentityN/Rank/Shape/ShapeN
的 proto 从 cann-ops-nn 迁到 cann-ops-math（control/ → conversion|math/），REG_OP 名不变。注意 Identity/Shape 的 dtype 由显式列表放宽为 `TensorType::ALL()`——旧工程按显式列表校验的适配代码可能行为变化，迁移核对须带上放宽点。
- **Reduce 族归并（api_redesign 8 对）**：BlockReduceSum/Max/Min→ReduceDataBlock、
WholeReduceSum/Max/Min 与 RepeatReduceSum→ReduceRepeat、PairReduceSum→ReducePairElem；归约操作改由 ReduceType 模板参选择，且 **mask 与 repeatTime 实参顺序对调**（旧 repeatTime 在前）——只改名不改调用形态编不过，逐对核签名差（RepeatReduceSum 还有 dstBlkStride 删除）。
- **文档改名批（renamed 15 对，`replacement_confidence` 经 devkit 双版本树比对已核为 `verified`）**：改名对为
头文件 update 注释与 interface_change_description.md 明示，版本未标注、生效版本由双版本安装树比对推断（新名 v9.0.0 在场）；多数旧名在 9.0/9.1 头内并存（deprecated 注释形态），但 ToBfloat16/ToFloat 在 9.0.0 起已不存在——8.x 代码原样上报时按此分流。


## deprecated 的日志识别形态

CANN 头文件中的废弃标记在编译日志中的表现形式：

```cpp
// 形态 1：[[deprecated]] 属性（编译 warning）
[[deprecated("use NewAPI instead")]] __aicore__ void OldAPI(...);
// 日志输出：
// warning: 'OldAPI' is deprecated: use NewAPI instead [-Wdeprecated-declarations]

// 形态 2：头文件 update 注释（无编译 warning，需 grep 头文件发现）
// @deprecated since 9.1.0, use NewAPI
// （不出现在编译日志中，需主动检索）

// 形态 3：条件编译移除（编译 error）
#if (__NPU_ARCH__ <= 2201)
// OldAPI removed in 9.1.0
#error "OldAPI has been removed; use NewAPI"
#endif
```

诊断引擎的 `api_version_map.yaml` 条目来自 `[[deprecated]]` 扫描；update 注释形态需
调用 CANNBOT `ascendc-docs-search` 技能来查官方迁移文档。8.x→9.0 方向的迁移对
尚未在随包引擎中登记（保持 unknown），需人工核实。
