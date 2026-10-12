// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
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
            std::fprintf(stderr, "GPU: no progress for %u s waiting for submission %llu (at %llu)\n",
                         seconds, (unsigned long long)tick,
                         (unsigned long long)gpu_tick.load(std::memory_order_relaxed));
        }
    }
    Refresh();
}

} // namespace Vulkan
