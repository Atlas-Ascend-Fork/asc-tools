/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_DIAGNOSTIC_REPORT_BUFFER_H
#define NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_DIAGNOSTIC_REPORT_BUFFER_H

#include "wire_protocol.h"

#include <cstddef>
#include <mutex>
#include <string>

namespace npucheck::diagnostic {

// 本次会话的报告聚合器。
//
// 诊断在 callback 线程上产生，退出路径上由 Finalize 一次性取走并发出，因此追加必须是
// 线程安全的。总长受 npucheck::ipc::MAX_RESULT_BYTES 约束：触及上限后停止追加并置 truncated，
// 而不是丢弃整份报告 —— 已经查出来的问题仍然要交付给用户，只是标明它不完整。
class ReportBuffer {
public:
    ReportBuffer() = default;
    ReportBuffer(const ReportBuffer&) = delete;
    ReportBuffer& operator=(const ReportBuffer&) = delete;

    // 追加一段报告文本。返回值表示是否全部写入：因触及上限而被截断时返回 false。
    // 运行在 callback 线程上，不允许抛异常；内存分配失败会被记为"报告不可用"。
    bool Append(const std::string& text) noexcept;

    // 取走全部文本并清空缓冲。只在退出路径上由单线程调用。
    std::string Take() noexcept;

    // 报告因触及总长上限被截断。对应线路上的 FLAG_TRUNCATED。
    bool Truncated() const noexcept;

    // 聚合过程中发生过内存分配失败，缓冲区内容已不可信。此时必须改发 Error，
    // 而不是把一份残缺的报告当成结论交出去。
    bool Failed() const noexcept;

    size_t Size() const noexcept;

private:
    mutable std::mutex mutex_;
    std::string buffer_;
    bool truncated_ = false;
    bool failed_ = false;
};

} // namespace npucheck::diagnostic

#endif // NPU_TOOLS_NPU_CHECK_SRC_PROCESSOR_DIAGNOSTIC_REPORT_BUFFER_H
