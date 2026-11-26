# 变长序列偏移单调性、跨张量维度契约与空指针校验指南

## 一、触发指纹与子例对比

- **典型报错**：`0x800000`（MTE 越界）、`0x8000`（标量解引用空指针或越界）、`0x10000`（负行长无符号回绕引发地址未对齐），或非法入参未返回 `161002` / `561002` / `GRAPH_FAILED`。
- **过/崩子例对比**：单调递增 `cu_seqlens` 且 `S1 <= S2` 的常规子例通过；传入非单调序列（如 `[0, 128, 24]` 产生负行长 `-104`）、末项超总行数 `T`、Q/KV 序列数组长度不一致、查询长度 `S1 > S2`、或必填/配对可选张量传 `nullptr` 时崩溃。
- **30 秒源码搜索词**：在 `op_api/` 与 `op_host/` 中搜 `actualSeqLengths`、`actual_seq`、`cu_seqlens`、`metadata_check`、`s1`、`s2`、`GetOptionalInputTensor`、`CHECK_RET`。

## 二、必查参数与公式清单

1. **必填与配对可选张量判空及标志位同步**：
   - 对照主控接口签名与核函数实际解引用点检查所有必填输入张量（如量化缩放因子 `smooth_scales`），为空直接在主控返回错误码；
   - 对成对出现的可选输入/输出（如前向附加项 `sinks` 与反向梯度 `dSinksOut`），校验配对合法性，并在 `TilingData` 中显式下发布尔开关（如 `isSink = (sinks != nullptr)`），核函数必须先判该开关再执行 `DataCopy`。
2. **双侧变长序列元素个数对齐**：同时存在 Q 侧与 KV 侧序列长度数组时，必须先校验 `qSeqLen.size() == kvSeqLen.size() && qSeqLen.size() == batchSize`，禁止直接拿 Q 侧长度做下标循环读取 KV 侧数组。
3. **累积序列（TND / 前缀和）单调性与总长边界**：
   - 首项约束：带前导零的 `cu_seqlens` 须满足 `seq[0] == 0`；无前导零的结束偏移数组须满足 `seq[0] >= 0`；
   - 单调与上界：对全部 `i >= 1` 必须满足 `seq[i] >= seq[i - 1]`，且末项 `seq.back() <= totalTokens`；
   - 核函数双保险：在核内计算单批有效行长 `rowCnt = seq[b + 1] - seq[b]` 后，守卫 `if (rowCnt <= 0) continue;`。
4. **跨张量序列关系契约**：对因果掩码、闪电索引（Lightning Indexer）等要求查询位置不超键值范围的算子，主控须严格校验每个批次及总维度满足 `S1 <= S2`（TND 模式下同步校验每段 `qLen_i <= kLen_i`）。
5. **子校验返回码透传**：检查主控入口调用的 `CheckSeqLen*`、`CheckShape*` 子函数返回值是否被赋给局部变量后漏写 `if (ret != SUCCESS) return ret;`。

## 三、常见误区（避坑）

- **误区 1**：把测试脚本中本就正常拦截的另一条异常子例误当成故障去改，浪费排查时间；开工先逐个子例核对测试脚本期望是跑通还是抛错。
- **误区 2**：只在核函数给负行长加 `return` 而不补主控校验，导致测试脚本断言 `GetWorkspaceSize` 返回 `161002` 失败。主控拦截与核内 `<= 0` 保护应同时具备。

## 四、标准修复代码模板

```cpp
// 主控侧（op_host / op_api）：校验跨张量序列长度关系与累积序列单调不减
static ge::graphStatus CheckVarlenAndSeqRelation(const int64_t* qSeq, size_t qCnt,
                                                 const int64_t* kSeq, size_t kCnt,
                                                 int64_t totalQ, int64_t totalK,
                                                 bool hasLeadingZero) {
    if (qSeq == nullptr || kSeq == nullptr || qCnt == 0 || qCnt != kCnt) {
        return ge::GRAPH_FAILED;
    }
    if (hasLeadingZero && (qSeq[0] != 0 || kSeq[0] != 0)) {
        return ge::GRAPH_FAILED;
    }
    int64_t prevQ = hasLeadingZero ? 0 : 0;
    int64_t prevK = hasLeadingZero ? 0 : 0;
    const size_t startIdx = hasLeadingZero ? 1 : 0;
    for (size_t i = startIdx; i < qCnt; ++i) {
        if (qSeq[i] < prevQ || kSeq[i] < prevK) return ge::GRAPH_FAILED; // 禁止非单调负行长
        const int64_t curQLen = qSeq[i] - prevQ;
        const int64_t curKLen = kSeq[i] - prevK;
        if (curQLen > curKLen) return ge::GRAPH_FAILED;                  // 契约要求 S1 <= S2
        prevQ = qSeq[i];
        prevK = kSeq[i];
    }
    if (prevQ > totalQ || prevK > totalK) return ge::GRAPH_FAILED;
    return ge::GRAPH_SUCCESS;
}
```
