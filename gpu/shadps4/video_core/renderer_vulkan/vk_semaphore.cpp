// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

#include "common/assert.h"

namespace Vulkan {

Semaphore::Semaphore(const Instance& instance_) : instance{instance_} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
}

Semaphore::~Semaphore() = default;

void Semaphore::Refresh() {
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        ASSERT_MSG(counter_result == vk::Result::eSuccess,
                   "Failed to get master semaphore value: {}", vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void Semaphore::Wait(u64 tick) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    // wbp: a lost device (a GPU hang reset by Windows' TDR) ends the wait with a clear error
    // instead of an endless loop behind a frozen picture; a GPU making no progress for 10 s is
    // reported in the log.
    constexpr u64 Second = 1'000'000'000ull;
    u32 seconds = 0;
    while (true) {
        const vk::Result result = instance.GetDevice().waitSemaphores(&wait_info, Second);
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            std::fprintf(stderr, "GPU: device lost while waiting for submission %llu (the GPU "
                                 "hung and the driver reset it)\n",
                         (unsigned long long)tick);
            ASSERT_MSG(false, "Device lost while waiting for a submission (GPU hang)");
        }
        if (result == vk::Result::eTimeout && ++seconds % 10 == 0) {
            const u64 used = instance.CanReportMemoryUsage() ? instance.GetDeviceMemoryUsage() >> 20 : 0;
            const u64 budget =
                instance.CanReportMemoryUsage() ? instance.GetDeviceMemoryBudgetNow() >> 20 : 0;
            Refresh();
            std::fprintf(stderr,
                         "GPU: no progress for %u s waiting for submission %llu (at %llu); video "
                         "memory %llu of %llu MiB\n",
                         seconds, (unsigned long long)tick,
                         (unsigned long long)gpu_tick.load(std::memory_order_relaxed),
                         (unsigned long long)used, (unsigned long long)budget);
            // wbp: one submission takes milliseconds; after this long the GPU will not get
            // there (a lost wait, a driver stuck paging memory). The game ends with the reason
            // instead of a frozen picture the player has to kill. BB_GPU_HANG_SECONDS=0: wait.
            static const u32 give_up = [] {
                const char* env = std::getenv("BB_GPU_HANG_SECONDS");
                return env ? u32(std::max(0, std::atoi(env))) : 30u;
            }();
            ASSERT_MSG(give_up == 0 || seconds < give_up,
                       "The GPU stopped responding: no progress for {} s (video memory {} of {} "
                       "MiB). Lower the output resolution or the graphics preset; if it keeps "
                       "happening, update the graphics driver.",
                       seconds, used, budget);
        }
    }
    Refresh();
}

} // namespace Vulkan
