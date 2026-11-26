# 算子固定规格上限、维度对齐倍数主控拦截与超大维度多趟拆分指南

## 一、触发指纹与子例对比

- **典型报错**：`0x4000000000000000` / `errcode ∈ {124, 154}`（UB 越界）、`0x10000`（`ADDR_MISALIGN` 地址未对齐）、`0x1000` / `507035`（`INSTR_ILLEGAL_CFG` 指令参数为 0）。
- **过/崩子例对比**：
  1. 阶数 `m=8192` 通过，`m=8193` 报 UB 越界；
  2. 权重维度 `N=64` 通过，非 8 倍数 `N=60` 报 MTE 地址未对齐；
  3. 旋转编码等算子 `headDim=4096` 通过，`headDim=8192` 或 `16384` 时单核 UB 可装行数 `ubCapacity / rowBytes` 整除塌缩为 `0` 报 `0x1000`。
- **30 秒源码搜索词**：在 `*_infershape.cpp`、`*_tiling.cpp`、`*_host.cpp` 与核函数头文件中搜 `TILE_LENGTH`、`8192`、`4096`、`% 8`、`% 16`、`% 32`、`maxUbSize`。

## 二、必查参数与公式清单

1. **核对算子设计规格上限（主控直接拦截）**：
   - 检查源码头文件、`infershape` 与核函数中是否存在写死的片上单列/单行缓冲常数或规格上限（例如分解/求逆算子固定 `TILE_LENGTH = 8192` 占满 192KB UB，或固定模板上限 `headDim <= 4096`；若工程附带说明文档可一并核对）；
   - 输入超出该规格上限时，在全部同族入口（如 `sgetrf`/`cgetrf` 或 `sgetri`/`cgetri` 的 `*_host.cpp`、`infershape` 与 `tiling`）统一返回 `ACL_ERROR_INVALID_PARAM`（`100000` / `161002`）或 `GRAPH_FAILED`。
2. **核对维度对齐倍数契约（主控直接拦截）**：
   - 分组矩阵乘量化、分形加载等算子对输出/权重列宽有固定对齐要求（如 `n > 0 && n % 8 == 0` 或 32B 对齐），解析出维度后立即在 `Tiling` / `GetWorkspaceSize` 校验，不满足直接返回 `GRAPH_FAILED`，不要误当成核函数尾块搬运去改。
3. **合法超大维度的整除塌缩保护与行内多趟循环**：
   - 若测试脚本要求超大维度（如 `headDim = 16384`，单行即超单块 UB 缓冲）**正常计算通过**，检查 `Tiling` 中 `rowsPerLoop = ubSize / (headDim * sizeof(T))` 是否整除变成了 `0`；
   - 当单行超出 UB 缓冲时，将 `rowsPerLoop` 固定为 `1`，并在 `Tiling` 与核函数中沿 `headDim` 方向按 32B 对齐的 `colTileElems` 做行内多趟分片循环（配套半旋转配对算子按前半/后半成对切分）。

## 三、常见误区（避坑）

- **误区 1**：遇到 `m=8193` 超出固定 `TILE_LENGTH=8192` 时，试图重写多块分治核函数，既耗时超时又违反接口返回错误码的测试断言。
- **误区 2**：修同族模板算子时只改了单精度入口（如 `sgetrf`），漏改复数或双精度入口（如 `cgetrf`）。修改前务必搜同目录下所有注册入口一并补齐。
- **误区 3**：修 `headDim` 超限时只拦了某一个 `tilingKey` 分支，漏掉 `infershape` 或另一个大尺寸分支。

## 四、标准修复代码模板

```cpp
// 模板 1：主控拦截超固定规格阶数与非对齐维度（op_host / infershape）
constexpr int64_t MAX_SUPPORTED_ORDER = 8192;
if (m <= 0 || m > MAX_SUPPORTED_ORDER || n <= 0 || (n % 8 != 0)) {
    return ge::GRAPH_FAILED; // aclnn 封装层对应返回 ACL_ERROR_INVALID_PARAM / 161002
}

// 模板 2：合法超宽单行防止 ubRows 整除变 0，切换行内多趟分片（op_host + op_kernel）
uint32_t rowBytes = headDim * sizeof(half);
if (rowBytes <= maxUbBufBytes) {
    tiling.set_rowsPerLoop(maxUbBufBytes / rowBytes); // 至少为 1
    tiling.set_colsPerPass(headDim);
} else {
    uint32_t maxCols = (maxUbBufBytes / sizeof(half)) & ~31U; // 保持 32B 对齐（half 为 16 元素倍数）
    tiling.set_rowsPerLoop(1);
    tiling.set_colsPerPass(maxCols); // 核函数内对 [0, headDim) 按 colsPerPass 分多趟搬运计算
}
```
