/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "diagnostic/report/report_catalog.h"

#include <string>

namespace npucheck::detail {
namespace {

constexpr char kRaceHazardTemplate[] =
    "========= {{Severity}}:[RACECHECK] {{raceKind}} race detected.\n"
    "=========     current access: {{currentAccess}} by aicore ({{secondCoreId}}) type ({{secondType}}) block "
    "({{secondBlock}}) "
    "pipe ({{secondPipe}}), range [0x{{currentRangeBegin}}, 0x{{currentRangeEnd}})\n"
    "=========     previous access: {{previousAccess}} by aicore ({{firstCoreId}}) type ({{firstType}}) block "
    "({{firstBlock}}) "
    "pipe ({{firstPipe}}), range [0x{{previousRangeBegin}}, 0x{{previousRangeEnd}})\n"
    "=========     memory: {{space}}{{memoryOwner}}, overlap [0x{{overlapBegin}}, 0x{{overlapEnd}}), "
    "{{overlapBytes}} bytes\n";

PatternCatalog::value_type MakePattern(
    ReportTool tool, NpuCheckReportPattern value, const char* name, const char* reportTemplate)
{
    return {{tool, name}, {value, ReportTemplate{reportTemplate}}};
}

const PatternCatalog& Catalog()
{
    // ToolManager::Finalize runs from the injection library's global destructor. The
    // catalog can be initialized by callback processing after that destructor is
    // registered, so a normal function-local static could be destroyed before
    // Finalize renders the final bundle. Keep this immutable catalog alive until
    // process exit instead.
    static const auto* const catalog = new PatternCatalog{
        MakePattern(
            ReportTool::MEMCHECK, NpuCheckReportPattern::MEMCHECK_INVALID_ACCESS, "invalid_access",
            "========= {{Severity}}:[MEMCHECK] Invalid {{space}} {{access}} of size {{accessBytes}} bytes\n"
            "=========     {{location}}\n"
            "=========     by aicore ({{coreId}}) type ({{blockType}}) block ({{blockId}}) pipe ({{pipeName}})\n"
            "=========     Address 0x{{address}} is out of bounds{{capacityContext}}\n"),
        MakePattern(
            ReportTool::MEMCHECK, NpuCheckReportPattern::MEMCHECK_MISALIGNED_ACCESS, "misaligned_access",
            "========= {{Severity}}:[MEMCHECK] Invalid {{space}} {{access}} of size {{accessBytes}} bytes\n"
            "=========     {{location}}\n"
            "=========     by aicore ({{coreId}}) type ({{blockType}}) block ({{blockId}}) pipe ({{pipeName}})\n"
            "=========     Address 0x{{address}} is misaligned\n"
            "=========     required alignment is {{requiredAlign}} bytes\n"),
        MakePattern(
            ReportTool::MEMCHECK, NpuCheckReportPattern::MEMCHECK_USE_AFTER_FREE, "use_after_free",
            "========= {{Severity}}:[MEMCHECK] Invalid {{space}} {{access}} of size {{accessBytes}} bytes\n"
            "=========     {{location}}\n"
            "=========     by aicore ({{coreId}}) type ({{blockType}}) block ({{blockId}}) pipe ({{pipeName}})\n"
            "=========     Address 0x{{address}} is used after free\n"
            "=========     and belongs to allocation at 0x{{base}} of size {{bytes}} bytes\n"),
        MakePattern(
            ReportTool::MEMCHECK, NpuCheckReportPattern::MEMCHECK_USE_BEFORE_ALLOC, "use_before_alloc",
            "========= {{Severity}}:[MEMCHECK] Invalid {{space}} {{access}} of size {{accessBytes}} bytes\n"
            "=========     {{location}}\n"
            "=========     by aicore ({{coreId}}) type ({{blockType}}) block ({{blockId}}) pipe ({{pipeName}})\n"
            "=========     Address 0x{{address}} is used before allocation is available\n"
            "=========     allocation 0x{{base}} of size {{bytes}} bytes was created at serial {{allocSerialNo}}\n"),
        MakePattern(
            ReportTool::MEMCHECK, NpuCheckReportPattern::MEMCHECK_INVALID_FREE, "invalid_free",
            "========= {{Severity}}:[MEMCHECK] Malloc/Free error encountered : Invalid pointer to free\n"
            "=========     at pc 0x{{pc}} in {{kernelName}}\n"
            "=========     by aicore ({{coreId}}) type ({{blockType}}) block ({{blockId}}) pipe ({{pipeName}})\n"
            "=========     Address 0x{{address}}\n"),
        MakePattern(
            ReportTool::MEMCHECK, NpuCheckReportPattern::MEMCHECK_DOUBLE_FREE, "double_free",
            "========= {{Severity}}:[MEMCHECK] Malloc/Free error encountered : Double free\n"
            "=========     at pc 0x{{pc}} in {{kernelName}}\n"
            "=========     by aicore ({{coreId}}) type ({{blockType}}) block ({{blockId}}) pipe ({{pipeName}})\n"
            "=========     Address 0x{{base}}\n"),
        MakePattern(
            ReportTool::MEMCHECK, NpuCheckReportPattern::MEMCHECK_LEAK, "leak",
            "========= {{Severity}}:[MEMCHECK] Leaked {{bytes}} bytes at 0x{{base}}\n"
            "=========     allocation id {{allocId}}, memory space {{space}}\n"),
        MakePattern(
            ReportTool::MEMCHECK, NpuCheckReportPattern::MEMCHECK_API_ERROR, "api_error",
            "========= {{Severity}}:[MEMCHECK] Program hit {{apiErrorName}} (error {{apiErrorCode}}) due to "
            "\"{{apiErrorMessage}}\" on ACL runtime API call to {{apiName}}.\n"),

        MakePattern(
            ReportTool::INITCHECK, NpuCheckReportPattern::INITCHECK_UNINITIALIZED_READ, "uninitialized_read",
            "========= {{Severity}}:[INITCHECK] Uninitialized {{space}} memory read of size {{accessBytes}} bytes\n"
            "=========     {{location}}\n"
            "=========     by aicore ({{coreId}}) type ({{blockType}}) block ({{blockId}}) pipe ({{pipeName}}) "
            "in launch ({{launchId}})\n"
            "=========     Address 0x{{address}}\n"),
        MakePattern(
            ReportTool::INITCHECK, NpuCheckReportPattern::INITCHECK_PARTIAL_UNINITIALIZED_READ,
            "partial_uninitialized_read",
            "========= {{Severity}}:[INITCHECK] Partially uninitialized {{space}} memory read of size "
            "{{accessBytes}} bytes\n"
            "=========     {{location}}\n"
            "=========     by aicore ({{coreId}}) type ({{blockType}}) block ({{blockId}}) pipe ({{pipeName}}) "
            "in launch ({{launchId}})\n"
            "=========     First uninitialized byte at 0x{{firstUninitAddress}}, {{uninitBytes}} bytes are "
            "uninitialized\n"),
        MakePattern(
            ReportTool::INITCHECK, NpuCheckReportPattern::INITCHECK_UNUSED_MEMORY, "unused_memory",
            "========= {{Severity}}:[INITCHECK] Unused {{space}} memory in allocation 0x{{base}} of size "
            "{{bytes}} bytes\n"
            "=========     Not written {{unusedBytes}} bytes at offset 0x{{firstUninitOffset}} "
            "(0x{{firstUninitAddress}})\n"
            "=========     {{unusedPercent}}% of allocation were unused.\n"),
        MakePattern(
            ReportTool::INITCHECK, NpuCheckReportPattern::INITCHECK_API_READ_UNINITIALIZED, "api_read_uninitialized",
            "========= {{Severity}}:[INITCHECK] Uninitialized {{space}} memory read by ACL runtime API\n"
            "=========     Address 0x{{address}}, size {{accessBytes}} bytes\n"),
        MakePattern(
            ReportTool::INITCHECK, NpuCheckReportPattern::INITCHECK_UNINITIALIZED_REGISTER_USE,
            "uninitialized_register_use",
            "========= {{Severity}}:[INITCHECK] Register {{registerName}} was used "
            "before initialization\n"
            "=========     {{consumerInstruction}} requires initializing register {{registerName}} before execution\n"
            "=========     {{location}}\n"
            "=========     by aicore ({{coreId}}) type ({{blockType}}) block ({{blockId}}) pipe ({{pipeName}}) "
            "in launch ({{launchId}})\n"),

        MakePattern(
            ReportTool::RACECHECK, NpuCheckReportPattern::RACECHECK_ANALYSIS, "analysis",
            "========= {{Severity}}:[RACECHECK] Race reported between {{firstAccess}} access at "
            "{{firstLocation}}\n"
            "=========     and {{secondAccess}} access at {{secondLocation}} "
            "[{{hazardCount}} hazards]\n"),
        MakePattern(
            ReportTool::RACECHECK, NpuCheckReportPattern::RACECHECK_HAZARD_RAW, "hazard_raw", kRaceHazardTemplate),
        MakePattern(
            ReportTool::RACECHECK, NpuCheckReportPattern::RACECHECK_HAZARD_WAR, "hazard_war", kRaceHazardTemplate),
        MakePattern(
            ReportTool::RACECHECK, NpuCheckReportPattern::RACECHECK_HAZARD_WAW, "hazard_waw", kRaceHazardTemplate),
        MakePattern(
            ReportTool::RACECHECK, NpuCheckReportPattern::RACECHECK_ATOMIC_SEMANTIC, "atomic_semantic",
            kRaceHazardTemplate),
        MakePattern(
            ReportTool::SYNCCHECK, NpuCheckReportPattern::SYNCCHECK_INTRA_CORE_DIVERGENT, "intra_core_divergent",
            "========= {{Severity}}:[SYNCCHECK] Barrier error detected. {{reason}}.\n"
            "=========     trigger point: {{triggerOperation}} at {{triggerLocation}}\n"
            "=========     by aicore ({{triggerCoreId}}) type ({{triggerType}}) block ({{triggerBlock}}) "
            "pipe ({{triggerPipe}})\n"
            "=========     scope {{scope}}, active mask 0x{{activeMask}}, expected mask 0x{{expectedMask}}\n"
            "{{objectLine}}"),
        MakePattern(
            ReportTool::SYNCCHECK, NpuCheckReportPattern::SYNCCHECK_INTER_CORE_DIVERGENT, "inter_core_divergent",
            "========= {{Severity}}:[SYNCCHECK] Barrier error detected. {{reason}}.\n"
            "=========     trigger point: {{triggerOperation}} by aicore ({{triggerCoreId}}) type ({{triggerType}}) "
            "block ({{triggerBlock}}) at "
            "{{triggerLocation}}\n"
            "{{relatedPointLine}}"
            "=========     scope {{scope}}, active mask 0x{{activeMask}}, expected mask 0x{{expectedMask}}\n"
            "{{objectLine}}"),
        MakePattern(
            ReportTool::SYNCCHECK, NpuCheckReportPattern::SYNCCHECK_INVALID_ARGUMENT, "invalid_argument",
            "========= {{Severity}}:[SYNCCHECK] Synchronization error detected. {{reason}}.\n"
            "=========     trigger point: {{triggerOperation}} by aicore ({{triggerCoreId}}) type ({{triggerType}}) "
            "block ({{triggerBlock}}) "
            "pipe ({{triggerPipe}}) at {{triggerLocation}}\n"
            "{{objectLine}}"),
        MakePattern(
            ReportTool::SYNCCHECK, NpuCheckReportPattern::SYNCCHECK_PAIRING_MISMATCH, "pairing_mismatch",
            "========= {{Severity}}:[SYNCCHECK] Synchronization pairing mismatch: {{reasonText}}.\n"
            "=========     trigger point: {{triggerOperation}} by aicore ({{triggerCoreId}}) type ({{triggerType}}) "
            "block ({{triggerBlock}}) "
            "pipe ({{triggerPipe}}) at {{triggerLocation}}\n"
            "{{relatedPointLine}}"
            "{{expectedOperationLine}}"
            "=========     pair kind {{pairKind}}, key ({{pairKey}})\n"),
        MakePattern(
            ReportTool::SYNCCHECK, NpuCheckReportPattern::SYNCCHECK_PARTICIPANT_MISMATCH, "participant_mismatch",
            "========= {{Severity}}:[SYNCCHECK] Synchronization error detected. {{reason}}.\n"
            "=========     trigger point: {{triggerOperation}} by aicore ({{triggerCoreId}}) type ({{triggerType}}) "
            "block ({{triggerBlock}}) "
            "pipe ({{triggerPipe}}) at {{triggerLocation}}\n"
            "{{relatedPointLine}}"
            "=========     scope {{scope}}, active mask 0x{{activeMask}}, expected mask 0x{{expectedMask}}\n"
            "{{objectLine}}"),
        MakePattern(
            ReportTool::SYNCCHECK, NpuCheckReportPattern::SYNCCHECK_DEADLOCK, "deadlock",
            "========= {{Severity}}:[SYNCCHECK] Deadlock detected. {{reason}}.\n"
            "=========     trigger point: {{triggerOperation}} by aicore ({{triggerCoreId}}) type ({{triggerType}}) "
            "block ({{triggerBlock}}) "
            "pipe ({{triggerPipe}}) at {{triggerLocation}}\n"
            "{{relatedPointLine}}"
            "=========     waiting mask 0x{{waitingMask}}, timeout {{timeoutNs}} ns\n"
            "{{objectLine}}"),
        MakePattern(
            ReportTool::SYNCCHECK, NpuCheckReportPattern::SYNCCHECK_OBJECT_NOT_INITIALIZED, "object_not_initialized",
            "========= {{Severity}}:[SYNCCHECK] Synchronization error detected. {{reason}}.\n"
            "=========     trigger point: {{triggerOperation}} by aicore ({{triggerCoreId}}) type ({{triggerType}}) "
            "block ({{triggerBlock}}) "
            "pipe ({{triggerPipe}}) at {{triggerLocation}}\n"
            "=========     primitive {{primitiveKind}}\n"
            "{{objectLine}}"),
        MakePattern(
            ReportTool::SYNCCHECK, NpuCheckReportPattern::SYNCCHECK_INSTRUCTION_SEQUENCE_MISMATCH,
            "instruction_sequence_mismatch",
            "========= {{Severity}}:[SYNCCHECK] Synchronization instruction sequence error detected. {{reason}}.\n"
            "=========     trigger point: {{triggerOperation}} by aicore ({{triggerCoreId}}) type ({{triggerType}}) "
            "block ({{triggerBlock}}) "
            "pipe ({{triggerPipe}}) at {{triggerLocation}}\n"
            "{{relatedPointLine}}"
            "=========     sequence index {{sequenceIndex}}, active mask 0x{{activeMask}}\n"),

        MakePattern(
            ReportTool::SOCCHECK, NpuCheckReportPattern::SOCCHECK_UNINITIALIZED_STATE_READ, "uninitialized_state_read",
            "========= {{Severity}}:[SOCCHECK] SOC state error detected. Uninitialized state read.\n"
            "=========     {{location}}\n"
            "=========     by aicore ({{coreId}}) type ({{blockType}}) block ({{blockId}}) pipe ({{pipeName}}) "
            "in launch ({{launchId}})\n"
            "=========     state kind {{stateKind}}, state id {{stateId}}, observed value 0x{{observedValue}}\n"),
        MakePattern(
            ReportTool::SOCCHECK, NpuCheckReportPattern::SOCCHECK_REGISTER_MISMATCH, "register_mismatch",
            "========= {{Severity}}:[SOCCHECK] SOC register mismatch detected.\n"
            "=========     {{location}}\n"
            "=========     by aicore ({{coreId}}) type ({{blockType}}) block ({{blockId}}) pipe ({{pipeName}}) "
            "in launch ({{launchId}})\n"
            "=========     register {{registerId}}, expected 0x{{expectedValue}}, observed 0x{{observedValue}}\n"),
        MakePattern(
            ReportTool::SOCCHECK, NpuCheckReportPattern::SOCCHECK_ILLEGAL_STATE_TRANSITION, "illegal_state_transition",
            "========= {{Severity}}:[SOCCHECK] SOC state error detected. Illegal state transition.\n"
            "=========     {{location}}\n"
            "=========     by aicore ({{coreId}}) type ({{blockType}}) block ({{blockId}}) pipe ({{pipeName}}) "
            "in launch ({{launchId}})\n"
            "=========     state id {{stateId}}, old value 0x{{oldValue}}, new value 0x{{newValue}}\n"),
        MakePattern(
            ReportTool::SOCCHECK, NpuCheckReportPattern::SOCCHECK_STATE_NOT_RESTORED, "state_not_restored",
            "========= {{Severity}}:[SOCCHECK] SOC state error detected. State was not restored.\n"
            "=========     at kernel exit for {{kernelName}} in launch ({{launchId}})\n"
            "=========     owner aicore ({{ownerCoreId}}), state id {{stateId}}\n"
            "=========     expected 0x{{expectedValue}}, observed 0x{{observedValue}}\n"),
        MakePattern(
            ReportTool::SOCCHECK, NpuCheckReportPattern::SOCCHECK_CROSS_CORE_STATE_INCONSISTENT,
            "cross_core_state_inconsistent",
            "========= {{Severity}}:[SOCCHECK] SOC state error detected. Cross-core state is inconsistent.\n"
            "=========     consumer aicore ({{consumerCoreId}}) in launch ({{consumerLaunchId}}) observed "
            "0x{{observedValue}}\n"
            "=========     producer aicore ({{producerCoreId}}) in launch ({{producerLaunchId}}) expected "
            "0x{{expectedValue}}\n"
            "=========     state id {{stateId}}, scope {{scope}}\n"),
        MakePattern(
            ReportTool::SOCCHECK, NpuCheckReportPattern::SOCCHECK_SCOPE_VIOLATION, "scope_violation",
            "========= {{Severity}}:[SOCCHECK] SOC state error detected. State scope violation.\n"
            "=========     {{location}}\n"
            "=========     by aicore ({{coreId}}) type ({{blockType}}) block ({{blockId}}) pipe ({{pipeName}}) "
            "in launch ({{launchId}})\n"
            "=========     state id {{stateId}}, invalid scope {{scope}}\n"),
    };
    return *catalog;
}

} // namespace

const PatternCatalog& GetPatternCatalog() { return Catalog(); }

const PatternDescriptor* FindPatternDescriptor(ReportTool tool, NpuCheckReportPattern pattern)
{
    for (const auto& [key, descriptor] : Catalog()) {
        if (key.tool == tool && descriptor.value == pattern) {
            return &descriptor;
        }
    }
    return nullptr;
}

const PatternDescriptor* FindPatternDescriptor(const ReportTemplateKey& key)
{
    const auto found = Catalog().find(key);
    return found == Catalog().end() ? nullptr : &found->second;
}

const char* PatternName(ReportTool tool, NpuCheckReportPattern pattern)
{
    for (const auto& [key, descriptor] : Catalog()) {
        if (key.tool == tool && descriptor.value == pattern) {
            return key.pattern.c_str();
        }
    }
    return "";
}

const char* ToolName(ReportTool tool)
{
    switch (tool) {
        case ReportTool::MEMCHECK:
            return "memcheck";
        case ReportTool::INITCHECK:
            return "initcheck";
        case ReportTool::RACECHECK:
            return "racecheck";
        case ReportTool::SYNCCHECK:
            return "synccheck";
        case ReportTool::SOCCHECK:
            return "soccheck";
    }
    return "unknown";
}

bool ParseToolName(const std::string& text, ReportTool* tool)
{
    if (tool == nullptr) {
        return false;
    }
    for (const ReportTool candidate :
         {ReportTool::MEMCHECK, ReportTool::INITCHECK, ReportTool::RACECHECK, ReportTool::SYNCCHECK,
          ReportTool::SOCCHECK}) {
        if (text == ToolName(candidate)) {
            *tool = candidate;
            return true;
        }
    }
    return false;
}

} // namespace npucheck::detail

namespace npucheck {

bool ReportTemplateKey::operator<(const ReportTemplateKey& other) const
{
    if (tool != other.tool) {
        return static_cast<int>(tool) < static_cast<int>(other.tool);
    }
    return pattern < other.pattern;
}

bool ReportTemplateKey::operator==(const ReportTemplateKey& other) const
{
    return tool == other.tool && pattern == other.pattern;
}

const ReportTemplate* FindBuiltinReportTemplate(const ReportTemplateKey& key)
{
    const detail::PatternDescriptor* descriptor = detail::FindPatternDescriptor(key);
    return descriptor == nullptr ? nullptr : &descriptor->reportTemplate;
}

std::vector<ReportTemplateKey> ListBuiltinReportTemplates()
{
    std::vector<ReportTemplateKey> keys;
    keys.reserve(detail::GetPatternCatalog().size());
    for (const auto& [key, descriptor] : detail::GetPatternCatalog()) {
        (void)descriptor;
        keys.push_back(key);
    }
    return keys;
}

const char* ReportToolName(ReportTool tool) { return detail::ToolName(tool); }

const char* ReportSeverityName(ReportSeverity severity)
{
    switch (severity) {
        case ReportSeverity::INFO:
            return "INFO";
        case ReportSeverity::WARNING:
            return "WARNING";
        case ReportSeverity::ERROR:
            return "ERROR";
    }
    return "ERROR";
}

} // namespace npucheck
