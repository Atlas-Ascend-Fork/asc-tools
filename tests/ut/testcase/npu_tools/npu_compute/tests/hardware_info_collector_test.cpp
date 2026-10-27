/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <gtest/gtest.h>

#include "hardware/hardware_info_collector.h"

#include <acl/acl_rt.h>
#include <acl/acl_platform.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <cstdlib>

namespace {

#define CHECK(expression)                                      \
    do {                                                       \
        if (!(expression)) {                                   \
            ADD_FAILURE_AT(__FILE__, __LINE__) << #expression; \
            return false;                                      \
        }                                                      \
    } while (false)

using namespace std::chrono_literals;

class TempDirectory {
public:
    TempDirectory()
    {
        std::string pathTemplate =
            (boost::filesystem::temp_directory_path() / "npu-compute-collector-test-XXXXXX").string();
        pathTemplate.push_back('\0');
        char* created = ::mkdtemp(pathTemplate.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~TempDirectory()
    {
        if (!path_.empty()) {
            boost::system::error_code error;
            boost::filesystem::remove_all(path_, error);
        }
    }

    const boost::filesystem::path& Path() const { return path_; }

private:
    boost::filesystem::path path_;
};

struct SharedState {
    std::atomic<int> hostCalls{0};
    std::atomic<int> deviceCountCalls{0};
    std::atomic<int> publishCalls{0};
    std::atomic<bool> hostSuccess{true};
    std::atomic<bool> publisherSuccess{true};
    std::mutex mutex;
    std::condition_variable condition;
    bool blockHost = false;
    bool hostStarted = false;
    bool releaseHost = false;
    std::thread::id hostThread;
    std::thread::id deviceThread;
    std::thread::id publishThread;
    std::vector<std::string> callOrder;
    std::string publishedJsonl;
    std::vector<std::string> diagnostics;
};

class FakeHardwareDeviceApi final : public npucompute::HardwareDeviceApi {
public:
    explicit FakeHardwareDeviceApi(std::shared_ptr<SharedState> state) : state_(std::move(state)) {}

    bool GetDeviceCount(std::int32_t* value) override
    {
        ++state_->deviceCountCalls;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->deviceThread = std::this_thread::get_id();
            state_->callOrder.emplace_back("device");
        }
        *value = 1;
        return true;
    }

    bool GetSocName(std::string* value) override
    {
        *value = "Ascend950PR_9599";
        return true;
    }

    bool GetDeviceAttribute(std::int32_t, std::int32_t attribute, std::int64_t* value) override
    {
        if (attribute == ACL_DEV_ATTR_NPU_ARCH) {
            *value = 3510;
        } else {
            *value = 1;
        }
        return true;
    }

    bool GetPlatformValue(std::int32_t type, std::string* value) override
    {
        *value = type == ACL_PLATFORM_MEMORY_SIZE ? "16777216" : "1000";
        return true;
    }

    bool GetControlCpuCount(std::int32_t, uint32_t* value) override
    {
        *value = 1;
        return true;
    }

    bool GetAiCpuFrequency(std::int32_t, uint32_t* value) override
    {
        *value = 1500;
        return true;
    }

    bool GetAiCoreFrequencies(std::int32_t, uint32_t* aicValue, uint32_t* aivValue) override
    {
        *aicValue = 1650;
        *aivValue = 1650;
        return true;
    }

    bool GetChipVersion(std::int32_t, std::string* value) override
    {
        *value = "V100";
        return true;
    }

    bool GetHbmUsage(std::int32_t, uint64_t* freeBytes, uint64_t* totalBytes) override
    {
        *freeBytes = 8U * 1024U * 1024U;
        *totalBytes = 16U * 1024U * 1024U;
        return true;
    }

    bool GetHbmFrequency(std::int32_t, uint32_t* value) override
    {
        *value = 3200;
        return true;
    }

private:
    std::shared_ptr<SharedState> state_;
};

npucompute::HardwareInfoPublishFunction MakePublisher(const std::shared_ptr<SharedState>& state)
{
    return [state](const boost::filesystem::path&, std::string_view jsonl, std::string* error) {
        ++state->publishCalls;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->publishThread = std::this_thread::get_id();
            state->callOrder.emplace_back("publish");
        }
        if (!state->publisherSuccess.load()) {
            if (error != nullptr) {
                *error = "injected publisher failure";
            }
            return npucompute::PublishResult::Failed;
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        state->publishedJsonl = std::string(jsonl);
        return npucompute::PublishResult::Published;
    };
}

npucompute::HardwareInfoDependencies MakeDependencies(const std::shared_ptr<SharedState>& state)
{
    npucompute::HardwareInfoDependencies dependencies;
    dependencies.collectHostInfo =
        [state](const boost::filesystem::path&, npucompute::HostInfo* value, npucompute::DiagnosticSink*) {
            ++state->hostCalls;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->hostThread = std::this_thread::get_id();
                state->callOrder.emplace_back("host");
                state->hostStarted = true;
                state->condition.notify_all();
                state->condition.wait(lock, [state] { return !state->blockHost || state->releaseHost; });
            }
            if (!state->hostSuccess.load()) {
                return false;
            }
            value->cpuPhysicalCount = 2;
            value->cpuLogicalCount = 16;
            value->memoryTotalSizeMb = 1024;
            value->diskTotalSizeGb = 100;
            return true;
        };
    dependencies.deviceApi = std::make_shared<FakeHardwareDeviceApi>(state);
    dependencies.diagnostics = [state](std::string_view message) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->diagnostics.emplace_back(message);
    };
    return dependencies;
}

bool TestCollectsOnTriggerThreadInOrderAndOnlyOnce()
{
    TempDirectory temporary;
    CHECK(!temporary.Path().empty());
    auto state = std::make_shared<SharedState>();
    npucompute::HardwareInfoCollector collector(MakeDependencies(state));
    std::string error;

    CHECK(collector.Initialize(temporary.Path(), MakePublisher(state), &error));
    CHECK(error.empty());
    CHECK(collector.State() == npucompute::HardwareCollectionState::WaitingKernel);
    CHECK(state->hostCalls.load() == 0);
    CHECK(state->deviceCountCalls.load() == 0);
    CHECK(state->publishCalls.load() == 0);

    const std::thread::id triggerThread = std::this_thread::get_id();
    collector.CollectOnKernelLaunch();
    collector.Stop();
    CHECK(collector.State() == npucompute::HardwareCollectionState::Completed);
    CHECK(state->hostCalls.load() == 1);
    CHECK(state->deviceCountCalls.load() == 1);
    CHECK(state->publishCalls.load() == 1);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        CHECK(state->hostThread == triggerThread);
        CHECK(state->deviceThread == triggerThread);
        CHECK(state->publishThread == triggerThread);
        CHECK(state->callOrder == std::vector<std::string>({"host", "device", "publish"}));
        CHECK(state->publishedJsonl.find("\"category\":\"Host Info\"") != std::string::npos);
        CHECK(state->publishedJsonl.find("\"category\":\"Device Info\"") != std::string::npos);
    }
    collector.CollectOnKernelLaunch();
    CHECK(state->hostCalls.load() == 1);
    CHECK(state->deviceCountCalls.load() == 1);
    CHECK(state->publishCalls.load() == 1);
    collector.Stop();
    return true;
}

bool TestConcurrentNotificationsStillCollectOnce()
{
    TempDirectory temporary;
    CHECK(!temporary.Path().empty());
    auto state = std::make_shared<SharedState>();
    npucompute::HardwareInfoCollector collector(MakeDependencies(state));
    CHECK(collector.Initialize(temporary.Path(), MakePublisher(state), nullptr));

    std::vector<std::thread> notifiers;
    for (int threadIndex = 0; threadIndex < 10; ++threadIndex) {
        notifiers.emplace_back([&collector] {
            for (int notification = 0; notification < 10; ++notification) {
                collector.CollectOnKernelLaunch();
            }
        });
    }
    for (auto& notifier : notifiers) {
        notifier.join();
    }
    collector.Stop();

    CHECK(collector.State() == npucompute::HardwareCollectionState::Completed);
    CHECK(state->hostCalls.load() == 1);
    CHECK(state->deviceCountCalls.load() == 1);
    CHECK(state->publishCalls.load() == 1);
    return true;
}

bool TestNotificationAndStopWaitForActiveCollection()
{
    TempDirectory temporary;
    CHECK(!temporary.Path().empty());
    auto state = std::make_shared<SharedState>();
    state->blockHost = true;
    npucompute::HardwareInfoCollector collector(MakeDependencies(state));
    CHECK(collector.Initialize(temporary.Path(), MakePublisher(state), nullptr));

    std::atomic<bool> notificationReturned{false};
    std::thread notifier([&collector, &notificationReturned] {
        collector.CollectOnKernelLaunch();
        notificationReturned = true;
    });

    bool hostStarted = false;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        hostStarted = state->condition.wait_for(lock, 10s, [state] { return state->hostStarted; });
        if (!hostStarted) {
            state->releaseHost = true;
        }
    }
    if (!hostStarted) {
        std::fprintf(
            stderr, "collector state at timeout: %d, host calls: %d\n", static_cast<int>(collector.State()),
            state->hostCalls.load());
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            for (const std::string& diagnostic : state->diagnostics) {
                std::fprintf(stderr, "collector diagnostic: %s\n", diagnostic.c_str());
            }
        }
        state->condition.notify_all();
        notifier.join();
        collector.Stop();
    }
    CHECK(hostStarted);

    std::atomic<bool> stopReturned{false};
    std::thread stopper([&collector, &stopReturned] {
        collector.Stop();
        stopReturned = true;
    });
    std::this_thread::sleep_for(20ms);
    const bool notificationReturnedBeforeRelease = notificationReturned.load();
    const bool returnedBeforeRelease = stopReturned.load();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->releaseHost = true;
    }
    state->condition.notify_all();
    notifier.join();
    stopper.join();

    CHECK(!notificationReturnedBeforeRelease);
    CHECK(notificationReturned.load());
    CHECK(!returnedBeforeRelease);
    CHECK(stopReturned.load());
    CHECK(collector.State() == npucompute::HardwareCollectionState::Completed);
    return true;
}

bool TestStopWithoutNotificationDoesNotCollect()
{
    TempDirectory temporary;
    CHECK(!temporary.Path().empty());
    auto state = std::make_shared<SharedState>();
    npucompute::HardwareInfoCollector collector(MakeDependencies(state));
    CHECK(collector.Initialize(temporary.Path(), MakePublisher(state), nullptr));
    collector.Stop();
    collector.CollectOnKernelLaunch();
    collector.Stop();

    CHECK(collector.State() == npucompute::HardwareCollectionState::NoKernelLaunch);
    CHECK(state->hostCalls.load() == 0);
    CHECK(state->deviceCountCalls.load() == 0);
    CHECK(state->publishCalls.load() == 0);
    CHECK(boost::filesystem::is_empty(temporary.Path()));
    return true;
}

bool TestCollectionAndPublishFailuresSetFailedState()
{
    TempDirectory temporary;
    CHECK(!temporary.Path().empty());
    auto hostFailure = std::make_shared<SharedState>();
    hostFailure->hostSuccess = false;
    npucompute::HardwareInfoCollector hostCollector(MakeDependencies(hostFailure));
    CHECK(hostCollector.Initialize(temporary.Path(), MakePublisher(hostFailure), nullptr));
    hostCollector.CollectOnKernelLaunch();
    hostCollector.Stop();
    CHECK(hostCollector.State() == npucompute::HardwareCollectionState::Failed);
    CHECK(hostFailure->hostCalls.load() == 1);
    CHECK(hostFailure->deviceCountCalls.load() == 0);
    CHECK(hostFailure->publishCalls.load() == 0);

    auto publishFailure = std::make_shared<SharedState>();
    publishFailure->publisherSuccess = false;
    npucompute::HardwareInfoCollector publishCollector(MakeDependencies(publishFailure));
    CHECK(publishCollector.Initialize(temporary.Path(), MakePublisher(publishFailure), nullptr));
    publishCollector.CollectOnKernelLaunch();
    publishCollector.Stop();
    CHECK(publishCollector.State() == npucompute::HardwareCollectionState::Failed);
    CHECK(publishFailure->hostCalls.load() == 1);
    CHECK(publishFailure->deviceCountCalls.load() == 1);
    CHECK(publishFailure->publishCalls.load() == 1);
    {
        std::lock_guard<std::mutex> lock(publishFailure->mutex);
        CHECK(!publishFailure->diagnostics.empty());
    }
    return true;
}

bool TestConcurrentNotificationsWaitForFailure()
{
    TempDirectory temporary;
    CHECK(!temporary.Path().empty());
    auto state = std::make_shared<SharedState>();
    state->blockHost = true;
    state->hostSuccess = false;
    npucompute::HardwareInfoCollector collector(MakeDependencies(state));
    CHECK(collector.Initialize(temporary.Path(), MakePublisher(state), nullptr));

    constexpr int kNotifierCount = 10;
    std::atomic<int> returnedCount{0};
    std::vector<std::thread> notifiers;
    for (int threadIndex = 0; threadIndex < kNotifierCount; ++threadIndex) {
        notifiers.emplace_back([&collector, &returnedCount] {
            collector.CollectOnKernelLaunch();
            ++returnedCount;
        });
    }

    bool hostStarted = false;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        hostStarted = state->condition.wait_for(lock, 10s, [state] { return state->hostStarted; });
    }
    if (hostStarted) {
        std::this_thread::sleep_for(20ms);
    }
    const int returnedBeforeRelease = returnedCount.load();

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->releaseHost = true;
    }
    state->condition.notify_all();
    for (auto& notifier : notifiers) {
        notifier.join();
    }

    CHECK(hostStarted);
    CHECK(returnedBeforeRelease == 0);
    CHECK(returnedCount.load() == kNotifierCount);
    CHECK(collector.State() == npucompute::HardwareCollectionState::Failed);
    CHECK(state->hostCalls.load() == 1);
    CHECK(state->deviceCountCalls.load() == 0);
    CHECK(state->publishCalls.load() == 0);
    return true;
}

} // namespace

static int RunSuiteMain()
{
    if (!TestCollectsOnTriggerThreadInOrderAndOnlyOnce() || !TestConcurrentNotificationsStillCollectOnce() ||
        !TestNotificationAndStopWaitForActiveCollection() || !TestStopWithoutNotificationDoesNotCollect() ||
        !TestCollectionAndPublishFailuresSetFailedState() || !TestConcurrentNotificationsWaitForFailure()) {
        return 1;
    }
    return 0;
}

TEST(NpuComputeHardwareInfoCollector, Main)
{
    ASSERT_EQ(RunSuiteMain(), 0) << "NpuComputeHardwareInfoCollector reported failure";
}
