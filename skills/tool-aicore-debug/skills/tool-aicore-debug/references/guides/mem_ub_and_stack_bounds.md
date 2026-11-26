# UB 多队列总容量预算、Vector 跨度掩码与核函数栈边界检查指南

## 一、触发指纹与子例对比

- **典型报错**：`0x4000000000000000`、`0x4000`、`errcode=154`（`vec_ub_addr_wrap_around`）、`0x1000` / `errcode=53`（`vec_mask_count_illegal`）。
- **过/崩子例对比**：小列宽或单缓冲通过；列宽增大使单行字节超 UB、开启 `BUFFER_NUM=2` 双缓冲后总和超 192KB、Vector 计算跨出已分配 `LocalTensor`、或掩码/重复次数为 `0` 时报错。
- **30 秒源码搜索词**：搜 `InitBuffer`、`AllocTensor`、`TBuf`、`TQue`、`maxUbSize`、`SetVectorMask`、`SetMaskCount`、`repeatTimes`。

## 二、必查参数与公式清单

1. **并发存活 `TBuf` / `TQue` 总字节预算公式**：
   - 汇总同一时刻存活的全部队列与临时缓冲：`sum(queueDepth_i * alignedTileBytes_i) + sum(tbufBytes_j) <= maxUbSize`（`GetLibApiWorkSpaceSize` 预留后剩余可用 UB，910B 物理上限通常不超 192KB，严禁调用不存在的 `SetUBSize`）；
   - 双缓冲队列必须乘以 `BUFFER_NUM`（`2`），且切勿漏算 `int8_t` / `uint8_t` 量化输出队列或中间 `float` 升精度缓冲；
   - 单行总字节 `numCol * pixelBytes` 超过可用 UB 时，`Tiling` 不能仅把单次行数保底置 `1` 了事，必须对列维 `numCol` 增加列向二次切分循环。
2. **严格区分字节单位与元素单位**：
   - `pipe.InitBuffer(que, depth, bytes)` 第三个参数是**字节数（且须 32B 对齐）**；
   - `que.AllocTensor<T>()` 与 Vector 双目/单目连续指令 `Add(dst, src0, src1, calCount)` 传的是**元素个数**，须保证 `calCount * sizeof(T) <= allocatedBytes`。
3. **Vector 掩码、重复次数与负偏移守卫（防 `errcode=53 / 154 / 0x4000`）**：
   - 高级重载 `repeatTimes` 必须落在 `[1, 255]` 内（超 255 须拆循环），且掩码不得全为 `0`；
   - 严禁对 `__ubuf__` / `LocalTensor` 使用负下标或下溢指针（触发 CCU 指令级地址检查 `0x4000`）。
4. **禁用核函数超大局部数组与递归**：`__aicore__` 函数不支持递归调用（编译器直接拒绝，须改写为循环），且禁止在栈上声明数 KB 局部数组（防止压穿标量栈，一律改用 `TBuf<TPosition::VECCALC>`）。

## 三、常见误区（避坑）

- **误区 1**：在 `CheckSplit` 中发现单行超 UB 后写 `rowsPerLoop = std::max(1, maxUb / rowBytes)`，结果 `1 * rowBytes` 依然大于 `maxUb`，核内 `InitBuffer` 直接溢出。必须同时拆列！
- **误区 2**：误把元素个数传给 `InitBuffer`（分少了 2~4 倍），或把字节数传给 `Add(..., count)`（多算了 2~4 倍写穿相邻缓冲）。

## 四、标准修复代码模板

```cpp
// 主控 Tiling：同时核算双缓冲 + 升精度缓冲 + 输出队列，超单行容量时启动列切分
uint64_t bytesPerElemBudget = 2 * sizeof(half)   // 输入 TQue 双缓冲
                            + sizeof(float)      // FP32 中间 TBuf
                            + 2 * sizeof(int8_t);// 量化输出 TQue 双缓冲
uint64_t maxColsInUb = (availableUbBytes / bytesPerElemBudget) & ~31ULL; // 32 元素对齐
if (static_cast<uint64_t>(numCol) <= maxColsInUb) {
    uint64_t rows = availableUbBytes / (static_cast<uint64_t>(numCol) * bytesPerElemBudget);
    tiling.set_rowsPerLoop(static_cast<uint32_t>(rows > 0 ? rows : 1));
    tiling.set_colsPerLoop(numCol);
} else {
    tiling.set_rowsPerLoop(1);
    tiling.set_colsPerLoop(static_cast<uint32_t>(maxColsInUb)); // 核内按列分块循环
}
```
