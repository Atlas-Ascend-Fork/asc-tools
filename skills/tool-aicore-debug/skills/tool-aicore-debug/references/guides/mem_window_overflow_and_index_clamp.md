# 大尺寸窗口乘法防溢出、有符号索引过滤与分页容量钳制指南

## 一、触发指纹与子例对比

- **典型报错与现象**：`0x4000000000000000` / `errcode=154`（UB 越界）、`0x800000` / `0x8000`（GM 越界），或 `exit_code=0` 静默产出越界索引（`indices_max >= kvCapacity`）。
- **过/崩子例对比**：
  1. **窗口 32 位溢出**：常规形状通过，单维超 `65536`（如 `W = 131073`）时池化/插值窗口报 UB 越界；
  2. **负索引零扩展**：合法非负下标通过，`indices` 含 `-1`、`padding_idx` 或 `>= num_weights`（测试要求正常返回 `0` 忽略越界项）时触发 `0x800000`；
  3. **分页容量未钳制**：`seqused_k` / `actual_seq_lengths_key` 超过物理块池总容量（尤其 `cmpRatio > 1` 放大后）静默产出越界索引；
  4. **分组/批次尾越界**：`group_index` 覆盖行数小于输入首维 `rowNum`，或 TND 分核尾块 `bIndex++` 后越过 `dimB` 解引用 `actualSeqLengths`。
- **30 秒源码搜索词**：在 `op_kernel/`（优先锁 `dav-c220 / arch22`）搜 `inLen`、`outLen`、`numWeights`、`paddingIdx`、`blockTable`、`seqused`、`cmpRatio`、`groupIdx`、`bIndex`。

## 二、必查参数与公式清单

1. **池化/插值起止下标禁直接算 32 位 `idx * inLen`**：当 `idx` 与 `inLen` 均为 `int32_t`/`uint32_t` 且 `inLen > 65536` 时，`idx * inLen` 会溢出 `2^31 - 1` 变成负数。必须全维度（D/H/W 起止点）改用 `int64_t` 或商余分解公式：
   - 设 `q = inLen / outLen`，`r = inLen % outLen`；
   - 起点 `start = idx * q + (idx * r) / outLen`；
   - 终点 `end = (idx + 1) * q + ((idx + 1) * r + outLen - 1) / outLen`。
2. **动态索引按有符号类型读取并双重过滤**：`int32_t` 负值 `-1` 若经 `uint32_t` 零扩展转 `uint64_t` 会变成 `0x00000000FFFFFFFF`，绕过 `== -1` 判断！必须先按 `int32_t`/`int64_t` 有符号读取符号扩展，再统一过滤 `idx < 0 || idx >= numWeights || idx == paddingIdx`（词嵌入梯度所有核变体如 `default`/`determinist`/`small_dim` 须同步修改）。
3. **分页键值缓存（Paged KV）有效长度按物理块池最终钳制**：
   - 主控 `Tiling` 提取物理块池总容量 `kvPhysCap = kvCache.GetShape().GetDim(0) * blockSize`（并与逻辑页表跨度 `blockTableCols * blockSize` 区分），写入 `TilingData`；
   - 核函数读取 `seqused_k` / `actual_seq_lengths_key` 后限制在 `[0, kvPhysCap]`，**若存在压缩比放大（如乘以 `cmpRatio`），必须在乘完 `cmpRatio` 之后的最终有效上界处再次与 `kvPhysCap` 取 `min`**；同时对读取出的 `blockId` 守卫 `0 <= blockId && blockId < kvBlockNum`。
4. **专家分组尾行与 TND 批次推进守卫**：推进 `groupIdx` 前检查 `groupIdx < groupNum`（未覆盖尾行按算子契约提前结束或置零，禁越界读 `smooth_scales`）；推进 `bIndex++` 后必须先判 `bIndex < dimB` 再读 `actualSeqLengths[bIndex]`。

## 三、常见误区（避坑）

- **误区 1**：对测试要求正常跑通的 `seqused_k` 超容量或 `embedding` 越界索引在主控返回 `161002` 报错，直接导致测试脚本异常退出失败。
- **误区 2**：把分页容量 `min` 钳制写在乘以 `cmpRatio` 之前，压缩比放大后最大索引依然越过物理容量。

## 四、标准修复代码模板

```cpp
// 1. 32 位安全窗口起止索引商余分解（防 idx * inLen 溢出）
__aicore__ inline int64_t SafeStartIdx(int64_t idx, int64_t outLen, int64_t inLen) {
    return idx * (inLen / outLen) + (idx * (inLen % outLen)) / outLen;
}
__aicore__ inline int64_t SafeEndIdx(int64_t idx, int64_t outLen, int64_t inLen) {
    return (idx + 1) * (inLen / outLen) + ((idx + 1) * (inLen % outLen) + outLen - 1) / outLen;
}

// 2. 核内有符号索引安全读取与范围过滤（杜绝 uint32 零扩展漏判 -1）
int64_t curIdx = isInt32Index ? static_cast<int64_t>(gmIndicesInt32.GetValue(pos))
                              : gmIndicesInt64.GetValue(pos);
if (curIdx < 0 || curIdx >= numWeights || curIdx == paddingIdx) {
    continue; // 跳过越界与填充索引，禁止写 GM
}

// 3. 分页有效键值长度在压缩比放大后的最终物理块池钳制
int64_t expandedSeqK = static_cast<int64_t>(rawSeqUsedK) * static_cast<int64_t>(cmpRatio);
if (expandedSeqK < 0) expandedSeqK = 0;
if (expandedSeqK > static_cast<int64_t>(kvPhysicalTokenCap)) {
    expandedSeqK = static_cast<int64_t>(kvPhysicalTokenCap);
}
```
