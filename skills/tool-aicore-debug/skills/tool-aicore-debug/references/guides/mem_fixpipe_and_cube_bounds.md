# Mmad 与 Fixpipe（L1 / L0A / L0B / L0C / FBUF / GM）参数与跨度检查指南

## 一、触发指纹与子例对比

- **典型报错**：`errcode=141`（`fixp_err_out_write_overflow`）、`0x1000000000000` / `0x1000000008000` / `0x4000000000` / `errcode=103`（L0A/L0B/L0C/L1/FBUF 越界）、`fixp_error0=0x9e` / `0xa0` / `errcode ∈ {101, 126}`（`Mmad` 或 `Fixpipe` 指令参数配置非法）。
- **过/崩子例对比**：单分块小矩阵乘通过；多 `ndNum` 批次累加偏移、非对齐 L0C 起始偏移、非 `NZ2ND` 模式漏填 `dstStride`、或 `Fixpipe` 写回跨出输出 GM 张量末址时报错。
- **30 秒源码搜索词**：搜 `Mmad`、`Fixpipe`、`FixpipeParamsV220`、`SetAtomicAdd`、`SetAtomicNone`、`dstStride`、`srcStride`、`ndNum`。

## 二、必查参数与公式清单

1. **真实 API 形态约束**：A2/A3 基础接口为 `AscendC::Fixpipe(dst, src, fp)`（参数结构为 `AscendC::FixpipeParamsV220`）；严禁臆造不存在的 `FixPipe(fp)` 或在 `dav-c220`（`__NPU_ARCH__==2201`）上使用仅新架构支持的 `isToUB` 字段。
2. **`errcode=141` 碰撞区分与目标 GM 矩阵跨度公式**：
   - 先追溯 `Fixpipe` 调用点是否处于 `SetAtomicAdd<T>()` 生效区间（未调 `SetAtomicNone()` 关闭则为原子累加写越界，否则为普通写回越界）；
   - 已确认 `NZ2ND`、`ROW_MAJOR` 写 ND 目标 GM 时：`rowBytes = nSize * sizeof(dstT)`，`rowStrideBytes = dstStride * sizeof(dstT)`，`matrixStrideBytes = dstNdStride * sizeof(dstT)`；
   - 多矩阵写入总字节跨度：`spanBytes = (ndNum - 1) * matrixStrideBytes + (mSize - 1) * rowStrideBytes + rowBytes`，必须满足 `dstOffsetBytes + spanBytes <= gmAllocBytes`（严禁用源端 `srcNdStride` 代替目标步长，也不能只看 `dstStride * ndNum`）。
3. **`FixpipeParamsV220` 字段合法域（防 `0x9e / 0xa0 / errcode=101 / 103`）**：
   - 非 `NZ2ND` 模式下当 `nSize > 16` 时 `dstStride` 严禁为 `0`；
   - `unitFlag` 只能取 `{0, 1, 2, 3}`；`isChannelSplit = true` 仅对 `float`（F32）输出合法；
   - 源端 L0C（容量 128KB）与量化/偏置 FBUF（容量 7KB）、L1（容量 512KB）按源分形布局独立核算 `(ndNum - 1) * srcNdStride + ...` 不越界。
4. **`Mmad` 参数与 L0A / L0B / L0C 边界（防 `errcode=126` 与回绕）**：
   - `Mmad` 要求 `m > 0 && k > 0 && n > 0`；
   - L0A 与 L0B 容量各 64KB，L0C 容量 128KB，循环累加时 `cL0c` 偏移须满足 16 元素（64B）对齐且 `offsetElems + mAligned * nAligned <= L0C_MAX_ELEMS`。

## 三、常见误区（避坑）

- **误区 1**：以为 `SetAtomicAdd()` 带目标地址参数；它只切换全局原子模式开关，真实写址完全取决于后续 `Fixpipe` / `DataCopy` 的目标张量切片。
- **误区 2**：尾块 `mTail` / `nTail` 未按实际剩余行列缩减 `FixpipeParamsV220.mSize / nSize`，把整块 `baseM * baseN` 写穿输出 GM 末尾。

## 四、标准修复代码模板

```cpp
// 尾块矩阵写回 GM：按实际剩余行列收敛 mSize / nSize，并配对复位原子累加状态
uint16_t actualM = static_cast<uint16_t>((mRemain < baseM) ? mRemain : baseM);
uint16_t actualN = static_cast<uint16_t>((nRemain < baseN) ? nRemain : baseN);
if (actualM > 0 && actualN > 0) {
    AscendC::FixpipeParamsV220 fp(actualN, actualM, srcStride, dstStride, false);
    // 若开启了原子加，确认目标地址对齐并在退出前恢复 SetAtomicNone()
    if (needAtomicAdd) {
        AscendC::SetAtomicAdd<float>();
    }
    AscendC::Fixpipe(gmDst[dstElemOffset], l0cLocal, fp);
    if (needAtomicAdd) {
        AscendC::SetAtomicNone();
    }
}
```
