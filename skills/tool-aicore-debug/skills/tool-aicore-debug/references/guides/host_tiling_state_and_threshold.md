# 主控切分全局状态污染、模板阈值漏等号与 64 位防溢出指南

## 一、触发指纹与子例对比

- **典型表现**：`exit_code=0` 但精度严重失真、尾块结果全 0，或触发非预期切分越界。
- **过/崩子例对比**：
  1. **临界点失真**：`K=1024` 和 `K=4096` 都通过，唯独卡在阈值点 `K=2048`（或整除 `tail == 0`）时精度崩坏；
  2. **跨调用污染**：单独运行失败子用例完全正常，但在同一进程连续跑完前置子用例后再跑它就报错或选错核函数；
  3. **双结构失步**：只更新了 `tilingData.matmulTiling`，未同步更新配套 `tilingData.params`（或函数末尾硬编码字面量覆盖了前面算好的字段）。
- **30 秒源码搜索词**：在 `op_host/` 全文搜文件级非 `const` 全局变量、`static `、`tilingKey`、`Special`、`> ` vs `>= `、`matmulTiling`。

## 二、必查参数与公式清单

1. **清除文件级全局与 `static` 可变切分状态**：`op_host` 切分函数在同一进程会被不同形状、不同接口分支反复调用；任何保存偏移、模式或 `tilingKey` 增量的可变全局/`static` 变量（如 `BENCHMARK_TILING_KEY`），一律移入每次调用局部零初始化的上下文结构体。
2. **逐处核查模板准入阈值 `<` 与 `<=`**：对照小分块模板的片上缓冲上限（如小模板最多容纳 `< 2048`，等于 `2048` 时已满溢覆写累加区），把误写的 `if (dim > kThreshold)` 修正为 `if (dim >= kThreshold)`。
3. **整除零尾块与双套 Tiling 结构同步**：
   - 尾块长度必须用 `tailLen = (total % tile == 0) ? tile : (total % tile)`，禁止把整除时的 `0` 下发给核函数；
   - 同时持有 `matmulTiling` 与自定义 `params` 结构体的算子，修改分块尺寸后两边字段必须同步写回，并检查函数末尾有无调试残留的硬编码赋值。
4. **64 位有界整数核算工作区与缓冲跨度**：主控计算 `workspace` 字节数及总元素数一律先转 `uint64_t` 并做乘法溢出检查；写回 `uint32_t` Tiling 字段前确认 `<= UINT32_MAX`。

## 三、常见误区（避坑）

- **误区 1**：遇到临界尺寸 `K=2048` 精度错时一头扎进几千行核函数逐行比对两个模板，忽略了 `op_host` 里只有一行的模板分发条件 `>` 漏了 `=`。先看主控分走了哪个 `tilingKey` 最快。
- **误区 2**：只用单子例命令验证就以为修好跨调用污染，漏跑同进程多形状连续调用回归。

## 四、标准修复代码模板

```cpp
#include <cstdint>
#include <limits>

// 1. Host 侧 64 位防溢出整数计算辅助（用于 workspace 与跨度核算，勿拷入核函数）
constexpr bool CheckedMulU64(uint64_t a, uint64_t b, uint64_t& out) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) return false;
    out = a * b;
    return true;
}
constexpr bool CheckedAddU64(uint64_t a, uint64_t b, uint64_t& out) {
    if (b > std::numeric_limits<uint64_t>::max() - a) return false;
    out = a + b;
    return true;
}

// 2. 消除可变全局状态 + 修正临界阈值漏等号 + 同步写回配套参数
struct PerCallTilingCtx {
    uint64_t tilingKeyOffset = 0; // 每次调用独立初始化为 0，禁止放文件级全局/static
};

if (s2Size >= kSpecialThreshold) { // 注意临界值 2048 必须归入大容量 Special 模板
    SelectSpecialTemplate(ctx, tilingData);
}
tilingData.params.set_baseM(tilingData.matmulTiling.get_baseM());
tilingData.params.set_baseN(tilingData.matmulTiling.get_baseN());
```
