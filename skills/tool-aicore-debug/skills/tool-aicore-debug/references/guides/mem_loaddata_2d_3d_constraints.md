# LoadData2D / LoadData3D 与 Nd2Nz 窗口、步长对齐及对称 Padding 检查指南

## 一、触发指纹与子例对比

- **典型报错**：`0x200000000`、`errcode ∈ {24, 26, 42, 47, 48, 93}`（L1/L0 搬运或 `Nd2Nz` 越界）、`0x10000` / `0x1000` / `errcode ∈ {35, 36}`（尺寸、步长或对齐非法）、`errcode=41` 伴随 `exit_code=124` 挂起（`padding_misconfig`）。
- **过/崩子例对比**：标准卷积/分形尺寸通过；小特征图配大卷积核导致补齐后仍小于核尺寸、通道数非 C0 对齐、步长为 0 或多块 `dstGap * repeatTimes` 超出 L0/UB 目标分配时报错或挂死。
- **30 秒源码搜索词**：搜 `LoadData2D`、`LoadData3D`、`LoadData3DParamsV2`、`Nd2Nz`、`padList`、`channelSize`、`strideW`、`kStartPt`。

## 二、必查参数与公式清单

1. **对称 Padding 几何可行性判据（防 `errcode=41` 核挂起）**：
   - `LoadData3D` 要求补齐后特征图尺寸不小于有效卷积核尺寸：`l1H + padTop + padBottom >= effFilterH` 且 `l1W + padLeft + padRight >= effFilterW`（其中膨胀核 `effFilter = (filter - 1) * dilation + 1`）；
   - 对称补齐最小合法值：`minPad = max(ceil(max(effFilterH - H, 0) / 2), ceil(max(effFilterW - W, 0) / 2))`；
   - 若 `minPad > maxAllowedPad`（几何上不可行），必须在主控 `Tiling` 直接返回失败错误码，**严禁对倒置区间执行 `clamp` 强行下发**；对原本已合法的 `pad` 严禁用 `min` 改小。
2. **`LoadData3D` 步长、通道对齐与起点边界（防 `errcode ∈ {24, 26, 35, 36, 47, 93}` 与静默错）**：
   - 步长与膨胀：`strideW >= 1 && strideH >= 1 && dilationW >= 1 && dilationH >= 1`（禁传 0）；
   - 通道对齐与 `c0`：`channelSize` 须满足当前类型 C0 对齐（`half`/`bfloat16` 为 16 对齐，`int8` 为 32 对齐），且 `c0` 索引不超源通道范围；
   - 窗口与面积：`kStartPt < filterK` 且按分形粒度对齐，`mStartPt` 不超有效输出窗口，源 L1 面积 `l1H * l1W * channelSize * sizeof(T) <= l1AllocBytes`。
3. **`LoadData2D` 与 `Nd2Nz` 读写跨度（防 `errcode ∈ {42, 48}`）**：
   - `LoadData2D` 每次搬运 512B 分形块，核对 `startIndex + repeatTimes * srcStride` 不超源 L1 分配，且目标端考虑 `dstGap` 后的末块不超 L0A/L0B（各 64KB）或目标缓冲；
   - `Nd2Nz` 核对源 GM 行列跨度与目标 L1 分形对齐后的 `align_up(D, c0) * align_up(N, 16) * sizeof(T)` 不超分配。

## 三、常见误区（避坑）

- **误区 1**：遇到 `exit_code=124` 超时只查 `SetFlag/WaitFlag`，漏看日志里的 `errcode=41` 或 `LoadData3D` 的 `filter > fmap + padding` 状态机挂死。
- **误区 2**：在 `dav-c220` 上部分非法 `kStartPt` 或超 L1 面积组合硬件可能不报专属码而表现为空输出或下游越界，须在主控 Tiling 前置校验。

## 四、标准修复代码模板

```cpp
// 主控 Tiling：校验 LoadData3D 参数合法域与 Padding 几何可行性
static bool ValidateLoad3DConfig(int64_t l1H, int64_t l1W, int64_t chSize, int64_t c0,
                                 int64_t filterH, int64_t filterW,
                                 int64_t strideH, int64_t strideW,
                                 int64_t padTop, int64_t padBottom,
                                 int64_t padLeft, int64_t padRight,
                                 uint64_t l1AllocBytes, size_t elemBytes) {
    if (l1H <= 0 || l1W <= 0 || chSize <= 0 || (chSize % c0) != 0) return false;
    if (strideH < 1 || strideW < 1 || filterH < 1 || filterW < 1) return false;
    if (l1H + padTop + padBottom < filterH || l1W + padLeft + padRight < filterW) {
        return false; // 不可行配置直接在主控拦截，防止触发 errcode=41 核挂死
    }
    const uint64_t needBytes = static_cast<uint64_t>(l1H) * l1W * chSize * elemBytes;
    return needBytes <= l1AllocBytes;
}
```
