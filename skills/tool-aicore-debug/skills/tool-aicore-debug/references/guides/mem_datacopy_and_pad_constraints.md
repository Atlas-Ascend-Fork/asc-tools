# DataCopy / DataCopyPad 跨度、32B 对齐与隐式补齐写穿检查指南

## 一、触发指纹与子例对比

- **典型报错**：`0x800000` / `errcode=1`（GM/L2 越界）、`0x4000000000000000` / `errcode ∈ {124, 125}`（MTE 读写 UB 越界，注意十进制标量 `errcode=124` 不是进程超时 `exit_code=124`）、`0x10000`（`ADDR_MISALIGN`）、`0x1000` / `errcode ∈ {30, 31}`（块长或块数非法）。
- **过/崩子例对比**：32 字节整倍数形状全部通过；遇到非 32B 对齐尾块、整除 `tail == 0` 仍走尾块搬运、多块带 `srcStride`/`dstStride` 跨度或开启 `DataCopyPad` 左右补齐时崩溃。
- **30 秒源码搜索词**：在 `op_kernel/` 中搜 `DataCopy`、`DataCopyPad`、`DataCopyParams`、`DataCopyExtParams`、`SetAtomicAdd`、`blockLen`、`blockCount`。

## 二、必查参数与公式清单

1. **非零块长与块数守卫（防 `errcode=30 / 31`）**：普通 `DataCopy` 要求 `blockCount > 0 && blockLen > 0`；尾块循环必须在 `remain > 0` 时才发射搬运指令，禁止整除 `tailLen == 0` 时下发 0 长度搬运。
2. **普通 `DataCopy(..., DataCopyParams)` 字节跨度公式**：
   - 以 32B 为单位的重载：单次访问跨度 `spanBytes = (blockCount * blockLen + (blockCount - 1) * gap) * 32`（源端代 `srcStride`，目标端代 `dstStride`）；
   - 按元素数传参的重载：`spanBytes = elemCount * sizeof(T)`，且 `spanBytes` 必须为 32B 整倍数；
   - 源与目标均须满足 `offsetBytes <= allocBytes && spanBytes <= allocBytes - offsetBytes`。
3. **`DataCopyPad`（GM -> UB）隐式 32B 行补齐公式（防 `errcode=124` 写穿 UB）**：
   - `DataCopyExtParams` 中 `blockLen` 单位为**字节**，`DataCopyPadExtParams<T>` 中 `leftPadding` / `rightPadding` 单位为**元素个数**；
   - 单行补齐后硬件会自动向上对齐到 32B：`rowBytes = AlignUp(blockLenBytes + (leftPadding + rightPadding) * sizeof(T), 32)`；
   - 目标 UB 至少须分配 `totalUbBytes = blockCount * rowBytes + (blockCount - 1) * dstStride * 32` 字节（不能只按未对齐字节数分配 UB）。
4. **类型化指针字节对齐与原子写 GM 对齐（防 `0x10000`）**：
   - 对类型化指针 `base + index`，真实字节地址是 `(uintptr_t)base + index * sizeof(*base)`，判断 32B 对齐绝不能只写 `index % 32 == 0`；
   - 开启 `SetAtomicAdd<T>()` 后向 GM 搬运，目标 GM 起始字节地址与写跨度同样必须满足对齐约束，并在写完后配对调用 `SetAtomicNone()`。

## 三、常见误区（避坑）

- **误区 1**：把 `stderr` 里的十进制 `errcode=124`（`datacopypad_ub_overflow` / `mte_write_ub_overflow`）当成 Linux `timeout` 的 `exit_code=124` 去查同步死锁。
- **误区 2**：臆造不存在的 `DataCopyPadParams<pad>` 模板；实际应按当前工程头文件配对使用 `DataCopyExtParams` + `DataCopyPadExtParams<T>`。

## 四、标准修复代码模板

```cpp
// 1. DataCopyPad GM -> UB 安全分配与非对齐尾块读取（防写穿 UB 与越界读 GM）
constexpr uint32_t ALIGN_32 = 32;
uint32_t validBytes = tailElems * sizeof(half);
uint32_t paddedBytes = validBytes + (leftPad + rightPad) * sizeof(half);
uint32_t alignedRowBytes = (paddedBytes + ALIGN_32 - 1) / ALIGN_32 * ALIGN_32;
// 初始化阶段 InitBuffer 分配容量必须 >= blockCount * alignedRowBytes + (blockCount - 1) * dstGap * 32

if (tailElems > 0) { // 严禁 tailElems == 0 时下发搬运
    AscendC::DataCopyExtParams copyParams{1, validBytes, 0, 0, 0};
    AscendC::DataCopyPadExtParams<half> padParams{true, leftPad, rightPad, static_cast<half>(0)};
    AscendC::DataCopyPad(ubLocal, gmSrc[tailOffset], copyParams, padParams);
}
```
