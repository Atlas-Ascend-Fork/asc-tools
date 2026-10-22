// ----------------------------------------------------------------------------------------------------------
// Copyright (c) 2026 Huawei Technologies Co., Ltd.
// This program is free software, you can redistribute it and/or modify it under the terms and conditions of
// CANN Open Software License Agreement Version 2.0 (the "License").
// Please refer to the License for details. You may not use this file except in compliance with the License.
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
// See LICENSE in the root of the software repository for the full text of the License.
// ----------------------------------------------------------------------------------------------------------

// timing_harness_aclrt_event.cpp — 短 kernel 计时模板（aclrtEvent 多 launch 单 event）
//
// 对应：REVIEW2 G4 / 整改 P0-2（计时 harness 货架）；ops-collect SKILL.md 计时口径表
// 「短 kernel（µs 级）launch 开销掩盖收益：stream 内多 launch 单 event 对计时」的
// 可拷贝实现。snapshot.py --kernel-avg-us 的第一生产者。
//
// 结构（固定，禁改动语义）：
//   warmup 轮剔除（默认 5，冷跑与稳态实测可差 25%）→ 稳态 repeat ≥3 轮
//   → 每轮 event 差值 → 取中位数（对离群轮稳健）。p50/p95/max 一并产出，
//   直接对齐 snapshot.py 可选分布字段（抖动诊断的数据源）。
//
// 集成方式（模板拷贝纪律：直接拷贝 + 最小适配，两处 [MODIFY]）：
//   1. 把本文件拷入算子工程（如 test/ 或根目录）；
//   2. [MODIFY-1] 替换 RUN_ONCE 为被测 kernel 的一次完整调用
//      （含全部输入输出分配与初始化——分配留在循环外，只计时 launch+执行）；
//   3. [MODIFY-2] 按 CMake 链接 aclrt / ascendcl；
//   4. 编译运行，result.json 即可直接喂 snapshot.py。
//
// 产物契约（opstune.timing/1.0）：kernel_avg_us = 稳态中位数；分布字段 p50/p95/max。

#include <acl/acl.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int32_t kDeviceId = 0;   // [MODIFY-2] 目标设备
constexpr int kWarmupDefault = 5;  // 固定 warmup 剔除（口径字段，须与 scope.json 一致）
constexpr int kRepeatDefault = 20; // 稳态轮数（口径字段；噪声带内复测时翻倍并同步重采基线）

// ---------------------------------------------------------------------------
// [MODIFY-1] 被测算子的一次完整调用：替换为真实的 kernel launch。
// 输入输出张量的分配与初始化应放在 setup()（循环外），此处只保留 launch，
// 否则计时里混入 malloc/初始化，口径失效。
// ---------------------------------------------------------------------------
struct BenchContext {
    aclrtStream stream = nullptr;
    // 示例占位：int8_t *devIn, *devOut; size_t inBytes, outBytes;
};

bool setup(BenchContext& ctx)
{
    // 示例形态（按算子实际 shape/dtype 修改）：
    //   aclrtMalloc(&ctx.devIn, ctx.inBytes, ACL_MEM_MALLOC_HUGE_FIRST);
    //   ... 初始化输入（host→device 拷贝）...
    (void)ctx;
    return true;
}

bool run_once(BenchContext& ctx)
{
    // 示例形态：
    //   return RunKernelLaunch(ctx.devIn, ctx.devOut, shape, ctx.stream) == 0;
    (void)ctx;
    return true;
}

void teardown(BenchContext& ctx)
{
    // 示例形态：aclrtFree(ctx.devIn); ...
    (void)ctx;
}

double median(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

double pct(std::vector<double>& v, double p)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(p * (v.size() - 1) + 0.5);
    return v[idx];
}

} // namespace

int main(int argc, char** argv)
{
    int warmup = kWarmupDefault, repeat = kRepeatDefault;
    const char* outPath = nullptr; // 必填 --output（结果路径由调用方指定）
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--warmup"))
            warmup = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--repeat"))
            repeat = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--output"))
            outPath = argv[i + 1];
    }
    if (outPath == nullptr) {
        std::fprintf(stderr, "ERROR: --output <path> 必填（结果 JSON 路径由调用方指定）\n");
        return 1;
    }
    if (repeat < 3) {
        std::fprintf(stderr, "ERROR: repeat >= 3（稳态采样下限，SKILL.md 计时口径）\n");
        return 1;
    }

    if (aclInit(nullptr) != ACL_SUCCESS) { /* 已初始化场景（宿主先 aclInit）容忍 */
    }
    ACL_CHECK(aclrtSetDevice(kDeviceId));
    BenchContext ctx;
    ACL_CHECK(aclrtCreateStream(&ctx.stream));
    if (!setup(ctx)) {
        std::fprintf(stderr, "ERROR: setup 失败（检查输入分配/初始化）\n");
        return 1;
    }

    // ---- warmup 轮（剔除冷跑：首次含初始化与缓存预热）----
    for (int i = 0; i < warmup; ++i) {
        if (!run_once(ctx)) {
            std::fprintf(stderr, "ERROR: warmup run 失败\n");
            return 1;
        }
    }
    ACL_CHECK(aclrtSynchronizeStream(ctx.stream));

    // ---- 稳态轮：多 launch 单 event 对计时（短 kernel：单 launch 的 event 分辨率
    //      与 launch 开销都会掩盖收益；一轮 = N 次 launch 用首尾 event 差值 / N）----
    constexpr int kLaunchPerRound = 10; // 每轮内 launch 数（按 kernel 时长调整：总时长 ≥ 100us 为宜）
    std::vector<double> perLaunchUs;
    aclrtEvent evStart, evEnd;
    ACL_CHECK(aclrtCreateEvent(&evStart));
    ACL_CHECK(aclrtCreateEvent(&evEnd));
    for (int r = 0; r < repeat; ++r) {
        ACL_CHECK(aclrtRecordEvent(evStart, ctx.stream));
        for (int i = 0; i < kLaunchPerRound; ++i) {
            if (!run_once(ctx)) {
                std::fprintf(stderr, "ERROR: steady run 失败\n");
                return 1;
            }
        }
        ACL_CHECK(aclrtRecordEvent(evEnd, ctx.stream));
        ACL_CHECK(aclrtSynchronizeStream(ctx.stream));
        float ms = 0.0f;
        ACL_CHECK(aclrtEventElapsedTime(&ms, evStart, evEnd));
        perLaunchUs.push_back(static_cast<double>(ms) * 1000.0 / kLaunchPerRound);
    }
    ACL_CHECK(aclrtDestroyEvent(evStart));
    ACL_CHECK(aclrtDestroyEvent(evEnd));

    // ---- 统计与落盘（opstune.timing/1.0；直接对齐 snapshot.py 输入字段）----
    std::vector<double> sorted = perLaunchUs;
    const double p50 = median(perLaunchUs);
    const double p95 = pct(sorted, 0.95);
    const double maxv = sorted.back();
    const double avg = p50; // 口径：kernel_avg_us = 稳态中位数（对离群轮稳健）

    std::ostringstream os;
    os << "{\n"
       << "  \"schema\": \"opstune.timing/1.0\",\n"
       << "  \"timing_method\": \"event\",\n"
       << "  \"harness\": \"aclrtEvent multi-launch single-pair\",\n"
       << "  \"launch_per_round\": " << kLaunchPerRound << ",\n"
       << "  \"warmup\": " << warmup << ",\n"
       << "  \"repeat\": " << repeat << ",\n"
       << "  \"kernel_avg_us\": " << avg << ",\n"
       << "  \"kernel_p50_us\": " << p50 << ",\n"
       << "  \"kernel_p95_us\": " << p95 << ",\n"
       << "  \"kernel_max_us\": " << maxv << ",\n"
       << "  \"per_round_us\": [";
    for (size_t i = 0; i < perLaunchUs.size(); ++i) {
        os << (i ? ", " : "") << perLaunchUs[i];
    }
    os << "]\n}\n";
    std::ofstream f(outPath);
    f << os.str();
    std::printf(
        "kernel_avg_us(p50)=%.3f  p95=%.3f  max=%.3f  (warmup=%d repeat=%d × %d launch)\n", avg, p95, maxv, warmup,
        repeat, kLaunchPerRound);
    std::printf("→ %s（喂 snapshot.py：--kernel-avg-us %.3f --timing-method event ...）\n", outPath, avg);

    teardown(ctx);
    ACL_CHECK(aclrtDestroyStream(ctx.stream));
    ACL_CHECK(aclrtResetDevice(kDeviceId));
    return 0;
}
