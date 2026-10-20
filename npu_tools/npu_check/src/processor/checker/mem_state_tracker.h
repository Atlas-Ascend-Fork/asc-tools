/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_MEM_STATE_TRACKER_H
#define NPU_TOOLS_NPU_CHECK_MEM_STATE_TRACKER_H

#include "diagnostic/report_message.h"
#include <array>
#include <cstddef>
#include <exception>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace npucheck {

enum class MemOpState : uint8_t { Write, AtomicWrite, Read, Race };
enum class RacecheckErrorKind {
    InvalidEvent,
    IncompleteTrace,
    UnsupportedEvent,
    MemoryAllocationFailed,
    CapacityExceeded
};

class RacecheckError : public std::exception {
public:
    // message 必须是静态字符串；异常构造不申请堆内存，内存不足时也能保留失败原因。
    RacecheckError(RacecheckErrorKind kind, const char* message) noexcept : kind_(kind), message_(message) {}
    const char* what() const noexcept override { return message_; }
    RacecheckErrorKind Kind() const noexcept { return kind_; }

private:
    RacecheckErrorKind kind_;
    const char* message_;
};

struct AccessPair {
    // Candidate evidence only; conflict classification belongs to synchronization analysis.
    CbIndex prevIndex;
    CbIndex curIndex;
    AclsanDeviceMemorySpace memorySpace;
    uint64_t begin;
    uint64_t end;
};
struct MemState {
    uint64_t begin;
    uint64_t end;
    MemOpState state;
    std::optional<CbIndex> writer; // 最近一次普通 WRITE；原子写不替换它。
    std::vector<CbIndex> readers;
    // 分别保存 ADD、MIN、MAX，兼容原子操作可直接跳过对应分组。
    std::array<std::vector<CbIndex>, 3> atomicWriters{};
};
using SegmentTable = std::map<uint64_t, MemState>;
using PhysicalSpaceTables = std::map<PhysicalSpaceKey, SegmentTable>;

// 详细事件默认关闭。未安装接收器时，错误写入 stderr。
// 接收器异常会被隔离，不能改变检查/更新结果。接收器
// 不得重入 tracker 或修改记录；message 仅在本次调用期间有效。
struct RacecheckDiagnostics {
    void (*emit)(void* context, const char* message) = nullptr;
    void* context = nullptr;
    // 未提供时错误交给 emit；两者均未提供时写 stderr。
    void (*emitError)(void* context, const char* message) = nullptr;
};

// tracker 和报告准备阶段共用的字节区间算法。
namespace racecheck {
struct ByteRange {
    uint64_t begin;
    uint64_t end;
};
// 单次访问解析结果，由调用方持有；各阶段按 const 引用使用，不存入历史节点。
struct PreparedAccess {
    PhysicalSpaceKey key;
    AclsanDeviceMemoryAccessMode mode;
    std::vector<ByteRange> ranges;
};
struct FailedRange {
    uint64_t begin;
    uint64_t end;
    size_t evidence;
};

PhysicalSpaceKey MakeKey(const AclsanDeviceMemoryAccessData& data);
AclsanDeviceMemoryAccessMode AccessMode(const AclsanDeviceMemoryAccessData& data);
inline bool IsAtomicWrite(AclsanDeviceMemoryAccessMode mode) noexcept
{
    return mode >= ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_ADD && mode <= ACLSAN_DEVICE_MEMORY_ACCESS_ATOMIC_WRITE_MAX;
}
inline bool CompatibleAtomicWrites(AclsanDeviceMemoryAccessMode previous, AclsanDeviceMemoryAccessMode current) noexcept
{
    return previous == current && IsAtomicWrite(current);
}
std::vector<ByteRange> ExpandAccess(const AclsanDeviceMemoryAccessData& data);
PreparedAccess PrepareAccess(const AclsanDeviceMemoryAccessData& data);
bool SameInstruction(const AclsanDeviceMemoryAccessData& lhs, const AclsanDeviceMemoryAccessData& rhs);
// 相同或按地址递增且不交叠的冲突区间线性整理；交叠/乱序输入使用通用扫描线。
// 保留各端点形成的边界和原始输入中最早的 evidence，不修改 conflicts。
std::vector<FailedRange> FailureRanges(const std::vector<AccessPair>& conflicts);
} // namespace racecheck

class MemStateTracker {
public:
    // records 的生命周期必须长于 tracker。已发布条目不可变，
    // 不能删除或重排。追加和调用必须串行；由于只保存下标和容器，
    // vector 扩容是安全的。
    explicit MemStateTracker(
        const std::vector<AclsanDeviceMemoryAccessData>& records, RacecheckDiagnostics diagnostics = {});
    // 第一次成功处理非空更新时，将此 tracker 绑定到一个 launch。
    // 集成层必须将每个 launch 路由到独立的 tracker。
    // 同一动态指令的源 READ 后接普通目标 WRITE 属于指令内在行为，
    // 不产生 WAR 候选。其他组合遵循访问矩阵，
    // 并需要外部判序。
    // 直接遍历重叠节点并按访问规则过滤，返回完整候选，不判断同步先后关系。
    std::vector<AccessPair> CheckPotentialRace(CbIndex cbIndex) const;
    // prepared 必须由同一条不可变 records[cbIndex] 解析，且覆盖整个串行处理过程。
    // 不保存 prepared 的引用；省略该参数的接口会在本次调用内自行解析。
    std::vector<AccessPair> CheckPotentialRace(CbIndex cbIndex, const racecheck::PreparedAccess& prepared) const;
    // 调用方在外部判序后提供完整且不变的已确认冲突候选子集，
    // 候选检查与更新之间不能修改表，不接受自行构造或已过期的冲突。
    // 所有模式直接修改原表，不再扫描历史复查候选成员关系。
    // 资源申请失败或修改开始后失败会清空并停止 tracker，调用方必须丢弃该 launch 的分析状态。
    void UpdateMemState(CbIndex cbIndex, const std::vector<AccessPair>& conflicts);
    void UpdateMemState(
        CbIndex cbIndex, const std::vector<AccessPair>& conflicts, const racecheck::PreparedAccess& prepared);
    const PhysicalSpaceTables& Tables() const noexcept { return tables_; }

private:
    std::vector<AccessPair> CheckPotentialRaceImpl(CbIndex cbIndex, const racecheck::PreparedAccess* prepared) const;
    void UpdateMemStateImpl(
        CbIndex cbIndex, const std::vector<AccessPair>& conflicts, const racecheck::PreparedAccess* prepared);
    const AclsanDeviceMemoryAccessData& Record(CbIndex index) const;

    const std::vector<AclsanDeviceMemoryAccessData>& records_;
    PhysicalSpaceTables tables_;
    RacecheckDiagnostics diagnostics_;
    std::optional<uint64_t> launchId_;
    std::optional<CbIndex> largestUpdatedIndex_;
    bool failed_ = false;
    void Trace(
        CbIndex index, const char* event, uint64_t begin = 0, uint64_t end = 0,
        CbIndex previous = static_cast<CbIndex>(-1), const char* detail = "", bool error = false) const noexcept;
    void TraceState(
        CbIndex index, const char* event, const MemState& state, const char* writerEvent = "history.writer",
        const char* readerEvent = "history.reader") const noexcept;
    void TraceTable(CbIndex index, const SegmentTable& table) const noexcept;
};
} // namespace npucheck
#endif
