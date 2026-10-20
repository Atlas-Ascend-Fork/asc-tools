/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "acl_san/aclsan_api.h"
#include "aclsan_register_dependency.h"
#include "checker/initcheck.h"
#include "diagnostic/pipeline_name.h"

#include "device_instr/common/instruction_id.h"

#include <chrono>
#include <cstddef>
#include <sstream>

namespace npucheck {
namespace {

constexpr size_t kMemoryAccessDependencySize =
    offsetof(AclsanDeviceMemoryAccessData, regDependencyMask1) + sizeof(uint64_t);
static_assert(ACLSAN_DEVICE_REGISTER_COUNT < 64);
constexpr uint64_t kSupportedRegisterMask = (UINT64_C(1) << static_cast<uint32_t>(ACLSAN_DEVICE_REGISTER_COUNT)) - 1;

uint64_t TimestampNs()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

struct RegisterDescription {
    const char* name;
    const char* setter;
};

RegisterDescription DescribeRegister(uint32_t id)
{
    static constexpr RegisterDescription descriptions[] = {
        {"vector_mask", "set_vector_mask"},
        {"mte2_source", "set_mte2_src_para"},
        {"nddma_pad_count", "set_pad_cnt_nddma"},
        {"nddma_loop0_stride", "set_loop0_stride_nddma"},
        {"nddma_loop1_stride", "set_loop1_stride_nddma"},
        {"nddma_loop2_stride", "set_loop2_stride_nddma"},
        {"nddma_loop3_stride", "set_loop3_stride_nddma"},
        {"nddma_loop4_stride", "set_loop4_stride_nddma"},
        {"mte2_nz", "set_mte2_nz_para"},
        {"loop3", "set_loop3_para"},
        {"dma_loop_size_ubuf_to_gm", "set_loop_size_ubtoout"},
        {"dma_loop_size_gm_to_ubuf", "set_loop_size_outtoub"},
        {"dma_loop_size_gm_to_cbuf", "set_loop_size_outtol1"},
        {"dma_loop1_stride_ubuf_to_gm", "set_loop1_stride_ubtoout"},
        {"dma_loop2_stride_ubuf_to_gm", "set_loop2_stride_ubtoout"},
        {"dma_loop1_stride_gm_to_ubuf", "set_loop1_stride_outtoub"},
        {"dma_loop2_stride_gm_to_ubuf", "set_loop2_stride_outtoub"},
        {"dma_loop1_stride_gm_to_cbuf", "set_loop1_stride_outtol1"},
        {"dma_loop2_stride_gm_to_cbuf", "set_loop2_stride_outtol1"},
        {"set_padding", "set_padding"},
        {"ctrl", "set_ctrl / set_atomic_*"},
    };
    return id < ACLSAN_DEVICE_REGISTER_COUNT ? descriptions[id] : RegisterDescription{"unknown", "unknown"};
}

const char* ConsumerInstructionName(uint32_t id)
{
    using aclsan::InstructionId;
    switch (static_cast<InstructionId>(id)) {
        case InstructionId::LoadGmToCbuf2DV2:
            return "load_gm_to_cbuf_2dv2";
        case InstructionId::CopyGmToCbufV2:
            return "copy_gm_to_cbuf_v2";
        case InstructionId::CopyGmToCbufAlignV2B8:
        case InstructionId::CopyGmToCbufAlignV2B16:
        case InstructionId::CopyGmToCbufAlignV2B32:
            return "copy_gm_to_cbuf_align_v2";
        case InstructionId::CopyGmToCbufMultiNd2NzB8:
        case InstructionId::CopyGmToCbufMultiNd2NzB16:
        case InstructionId::CopyGmToCbufMultiNd2NzB32:
            return "copy_gm_to_cbuf_multi_nd2nz";
        case InstructionId::CopyGmToCbufMultiDn2NzB8:
        case InstructionId::CopyGmToCbufMultiDn2NzB16:
        case InstructionId::CopyGmToCbufMultiDn2NzB32:
            return "copy_gm_to_cbuf_multi_dn2nz";
        case InstructionId::CopyUbufToGmAlignV2:
            return "copy_ubuf_to_gm_align_v2";
        case InstructionId::CopyGmToUbufAlignV2B8:
        case InstructionId::CopyGmToUbufAlignV2B16:
        case InstructionId::CopyGmToUbufAlignV2B32:
            return "copy_gm_to_ubuf_align_v2";
        case InstructionId::NdDmaOutToUbufB8:
        case InstructionId::NdDmaOutToUbufB16:
        case InstructionId::NdDmaOutToUbufB32:
            return "nd_copy_gm_to_ubuf";
        case InstructionId::FixL0cToOutF32:
        case InstructionId::FixL0cToOutS32:
            return "fix_l0c_to_out";
        default:
            return "unknown";
    }
}

NpuCheckReportExecContext ToExecContext(const AclsanDeviceMemoryAccessData& data)
{
    NpuCheckReportExecContext exec{};
    exec.launchId = data.header.launchId;
    exec.instrExecId = data.header.instrExecId;
    exec.serialNo = data.header.serialNo;
    exec.pc = data.header.pc;
    exec.deviceId = data.header.deviceId;
    exec.phyCoreId = data.header.phyCoreId;
    exec.blockId = data.header.blockId;
    exec.blockType = data.header.blockType;
    exec.pipeId = data.header.pipeline;
    exec.siteId = data.header.siteId;
    exec.pipeName = detail::DevicePipelineName(data.header.pipeline);
    return exec;
}

} // namespace

const std::vector<CallbackSpec>& Initcheck::GetSubscribedID() const
{
    static const std::vector<CallbackSpec> callbacks{
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_STATE},
        {ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION, ACLSAN_CBID_DEVICE_MEMORY_ACCESS},
        {ACLSAN_CB_DOMAIN_LAUNCH, ACLSAN_CBID_LAUNCH_KERNEL},
        {ACLSAN_CB_DOMAIN_SYNCHRONIZE, ACLSAN_CBID_SYNCHRONIZE_STREAM_SYNC_END}};
    return callbacks;
}

bool Initcheck::OnRegisterState(const AclsanDeviceRegisterStateData& data)
{
    if (data.header.size < sizeof(data) || data.regId >= ACLSAN_DEVICE_REGISTER_COUNT || data.regId >= 64) {
        ++stats_.malformedEvents;
        return false;
    }
    const CoreKey key{data.header.deviceId, data.header.launchId, data.header.blockType, data.header.blockId};
    initializedRegisters_[key] |= aclsan::RegisterBit(static_cast<AclsanDeviceRegisterId>(data.regId));
    ++stats_.registerWrites;
    return true;
}

NpuCheckInitcheckReport Initcheck::MakeReport(
    const AclsanDeviceMemoryAccessData& data, uint32_t registerId, uint64_t groupId)
{
    const RegisterDescription description = DescribeRegister(registerId);
    NpuCheckInitcheckReport report{};
    report.common.reportId = nextReportId_++;
    report.common.groupId = groupId;
    report.common.timestampNs = TimestampNs();
    report.common.tool = ReportTool::INITCHECK;
    report.common.severity = ReportSeverity::ERROR;
    report.common.pattern = NpuCheckReportPattern::INITCHECK_UNINITIALIZED_REGISTER_USE;
    report.common.flags = kNpuCheckReportCommonHasExecContext;
    report.common.exec = ToExecContext(data);
    report.registerId = registerId;
    report.registerName = description.name;
    report.setterInstruction = description.setter;
    report.consumerInstruction = ConsumerInstructionName(data.instructionId);
    return report;
}

bool Initcheck::OnMemoryAccess(const AclsanDeviceMemoryAccessData& data, CheckerReportList& reports)
{
    if (data.header.size < kMemoryAccessDependencySize || data.regDependencyMask1 != 0 ||
        (data.regDependencyMask0 & ~kSupportedRegisterMask) != 0) {
        ++stats_.malformedEvents;
        return false;
    }
    ++stats_.deviceOperations;
    const CoreKey core{data.header.deviceId, data.header.launchId, data.header.blockType, data.header.blockId};
    const uint64_t initialized = initializedRegisters_[core];
    uint64_t missing = 0;
    for (uint32_t id = 0; id < ACLSAN_DEVICE_REGISTER_COUNT; ++id) {
        const auto registerId = static_cast<AclsanDeviceRegisterId>(id);
        if (aclsan::HasRegisterBit(data.regDependencyMask0, registerId) &&
            !aclsan::HasRegisterBit(initialized, registerId)) {
            missing |= aclsan::RegisterBit(registerId);
        }
    }
    if (missing == 0) {
        return true;
    }

    const InstructionKey instruction{
        data.header.deviceId, data.header.launchId, data.header.blockType, data.header.blockId,
        data.header.instrExecId};
    const auto [group, inserted] = reportGroups_.try_emplace(instruction, nextGroupId_);
    if (inserted) {
        ++nextGroupId_;
    }
    while (missing != 0) {
        const uint32_t registerId = static_cast<uint32_t>(__builtin_ctzll(missing));
        missing &= missing - 1;
        const UsageKey usage{data.header.deviceId, data.header.launchId,    data.header.blockType,
                             data.header.blockId,  data.header.instrExecId, registerId};
        if (!reportedUsages_.insert(usage).second) {
            continue;
        }
        reports.emplace_back(MakeReport(data, registerId, group->second));
        ++stats_.errors;
    }
    return true;
}

void Initcheck::OnSynchronization(const AclsanSynchronizeData& data)
{
    ++stats_.synchronizations;
    if (data.common.result != 0) {
        ++stats_.failedSynchronizations;
        return;
    }
    initializedRegisters_.clear();
    reportedUsages_.clear();
    reportGroups_.clear();
}

bool Initcheck::OnCallback(
    AclsanCallbackDomain domain, AclsanCallbackId cbid, const void* data, CheckerReportList& reports)
{
    if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && cbid == ACLSAN_CBID_DEVICE_STATE) {
        return OnRegisterState(*static_cast<const AclsanDeviceRegisterStateData*>(data));
    }
    if (domain == ACLSAN_CB_DOMAIN_DEVICE_INSTRUCTION && cbid == ACLSAN_CBID_DEVICE_MEMORY_ACCESS) {
        return OnMemoryAccess(*static_cast<const AclsanDeviceMemoryAccessData*>(data), reports);
    }
    if (domain == ACLSAN_CB_DOMAIN_LAUNCH) {
        const auto* event = static_cast<const AclsanLaunchData*>(data);
        if (event->common.result != 0) {
            ++stats_.failedLaunches;
        }
        return true;
    }
    if (domain == ACLSAN_CB_DOMAIN_SYNCHRONIZE) {
        OnSynchronization(*static_cast<const AclsanSynchronizeData*>(data));
    }
    return true;
}

InitcheckStats Initcheck::Stats() const { return stats_; }

bool Initcheck::HasErrors() const { return stats_.errors != 0; }

bool Initcheck::AnalysisComplete() const
{
    return stats_.malformedEvents == 0 && stats_.failedLaunches == 0 && stats_.failedSynchronizations == 0;
}

std::string Initcheck::Summary() const
{
    std::ostringstream output;
    output << "tool=initcheck register_writes=" << stats_.registerWrites
           << " device_operations=" << stats_.deviceOperations << " synchronizations=" << stats_.synchronizations
           << " errors=" << stats_.errors << " malformed_events=" << stats_.malformedEvents
           << " failed_launches=" << stats_.failedLaunches
           << " failed_synchronizations=" << stats_.failedSynchronizations;
    return output.str();
}

} // namespace npucheck
