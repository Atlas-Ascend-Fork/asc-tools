# load3d_mte_param_check — Load3D / MTE 参数语义检查（dav-2201 静默）

## 基本信息

| 属性 | 值 |
|------|---|
| 异常编号 | load3d_mte_param_check |
| 异常类型 | 静态预防（dav-2201 参数语义检查省略） |
| 错误码 | 无（硬件静默，stderr=0 字节） |
| 类别 | static_preventive |
| 性质 | 预防型 |
| 覆盖条目 | C-2：L38 FPOS_LARGER_FSIZE / L39 FMAPWH_LARGER_L1SIZE / L40 filter>fmap+padding / L60 MTE L1→UB 写溢出（4 条） |
| 检测时机 | host tiling 计算 / kernel 编译期 |
| 报错模式 | 无（dav-2201 实机验证硬件静默） |

## 触发条件

历史资料记录 dav-2201 上部分 LoadData3D / MTE 参数组合未上报目标异常；不能推广为所有重载或非法组合都静默。runtime detector 的有码路径须按对应种子、版本和日志分别验证。

- **违规模式**：LoadData3D 的 fetch position 超 filter 范围 / FeatureMap 面积超 L1 / filter > fmap+padding / MTE L1→UB 写地址溢出
- **安全模式**：host 侧 tiling 计算添加参数合法性校验作为补偿层

## 检测规则

### load3d_mte_param_check-RULE-01：fetch position 超 filter 范围（L38）

【条件】`LoadData3DParamsV2` 中 `kStartPt >= filterK`
【API】`LoadData3D`（kStartPt 参数）
【约束】`kStartPt < filterK`
【匹配模式】值域判断

### load3d_mte_param_check-RULE-02：FeatureMap 面积超 L1（L39）

【条件】`l1H × l1W × channelSize × sizeof(T) > L1_SIZE (512KB)`
【API】`LoadData3D`（l1H/l1W/channelSize 参数）
【约束】`l1H × l1W × channelSize × sizeof(T) <= L1_SIZE`
【匹配模式】值域判断

### load3d_mte_param_check-RULE-03：filter 尺寸 > fmap + padding（L40）

【条件】`filterW > l1W + padLeft + padRight` 或 `filterH > l1H + padTop + padBottom`
【API】`LoadData3D`（filterW/filterH/padding 参数）
【约束】`filterW <= l1W + padLeft + padRight` 且 `filterH <= l1H + padTop + padBottom`
【匹配模式】值域判断

### load3d_mte_param_check-RULE-04：MTE L1→UB 写地址溢出（L60）

【条件】DataCopy dst UB 地址 + 搬运量 > `GetUBSize()`
【API】`DataCopy`（dst UB 偏移 + 搬运量）
【约束】`dstOffset + copySize <= UB_SIZE`
【匹配模式】值域判断

## 修复方案

### 标准修复模式

1. host 侧 tiling 计算中添加参数校验断言
2. kernel 启动前检查 LoadData3D 参数合法性
3. 编译期约束：`static_assert(dstOffset + copySize <= UB_SIZE)`

### 代码模板

```cpp
// 预防型：host 侧 tiling 参数校验（L38/L39/L40）
void ValidateLoad3DParams(const LoadData3DParamsV2& params, uint32_t filterK,
                          uint32_t l1H, uint32_t l1W, uint32_t channelSize) {
    // L38: fetch position
    ASSERT(params.kStartPt < filterK);
    // L39: FeatureMap 面积
    uint64_t fmapBytes = static_cast<uint64_t>(l1H) * l1W * channelSize * sizeof(float);
    ASSERT(fmapBytes <= L1_SIZE_BYTES);  // 512KB
    // L40: filter vs fmap + padding
    ASSERT(params.filterW <= l1W + params.padLeft + params.padRight);
    ASSERT(params.filterH <= l1H + params.padTop + params.padBottom);
}
```

```cpp
// 预防型：MTE L1→UB 边界校验（L60）
__aicore__ inline void SafeDataCopyL1ToUB(
    LocalTensor<float>& ubDst, uint32_t dstOffset,
    LocalTensor<float>& l1Src, uint32_t srcOffset, uint32_t copySize) {
    // L60: 编译期 + 运行时双重校验
    static_assert(sizeof(float) == 4, "");
    ASSERT(dstOffset + copySize <= UB_SIZE_FLOATS);
    DataCopy(ubDst[dstOffset], l1Src[srcOffset], copySize);
}
```

### 防御性编程建议

1. tiling 计算函数返回前调用 `ValidateLoad3DParams`
2. DataCopy L1→UB 方向强制加 `ASSERT(dstOffset + copySize <= UB_SIZE)`
3. 后续芯片版本可重试这些检查（当前 dav-2201 省略）

## 变体索引

| 变体 | 路径 | 触发方式 | 等价类 |
|------|------|---------|--------|
| FPOS_LARGER_FSIZE | C-2 L38 | kStartPt >= filterK，硬件静默 | EC1 |
| FMAPWH_LARGER_L1SIZE | C-2 L39 | fmap 面积超 L1，硬件静默 | EC2 |
| filter>fmap+padding | C-2 L40 | filter 超 fmap+padding，硬件静默 | EC3 |
| MTE L1→UB 写溢出 | C-2 L60 | DataCopy dst UB 超限，硬件静默 | EC4 |

## 等价类划分

| 等价类 | 描述 | 判据 |
|--------|------|------|
| EC1 | kStartPt 合法 | kStartPt < filterK |
| EC2 | fmap 面积合法 | l1H×l1W×channelSize×sizeof ≤ L1_SIZE |
| EC3 | filter 合法 | filter ≤ fmap + padding |
| EC4 | UB 写合法 | dstOffset + copySize ≤ UB_SIZE |

## 关联异常

- **padding_misconfig**（runtime）：L40 的运行时表现（有错误码 41 但实际挂起）
- **fmap_size_illegal** / **first_window_overflow** / **l1_3d_size_illegal**（runtime）：LoadData3D 参数类异常
- **load3d_km_start_illegal** / **load3d_km_step_illegal**（runtime）：LoadData3D km 参数类异常

## 检测执行指引

- **适用匹配模式**：值域判断（全部 RULE）
- **命中判定**：参数值违反已确认约束时命中；host 侧 tiling 校验缺失仅为防御建议，不单独证明越界或挂起；单位和容量按 references/constraints/semantic_contract.md 确认
- **修复映射**：RULE-01/02/03 → host 侧 tiling 校验断言；RULE-04 → DataCopy 边界校验
- **trace_limited 判定**：参数来自 host→kernel 传播链时，跨边界追溯 → `trace_limited=true`
- **runtime_required 判定**：参数值依赖运行时 tiling → `runtime_required=true`
