// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>

#include <condition_variable>

#include "core/libraries/videoout/buffer.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Image image;
    vk::ImageView image_view;
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    bool is_hdr{false};
    u8 id{};
    /// wbp: FSR 3 frame generation: the frame interpolated before this one, shown first.
    Frame* generated{nullptr};

};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;

class Presenter {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter();

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    HostPasses::FsrPass::Settings& GetFsrSettingsRef() {
        return fsr_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) {
        if (!IsHDRSupported()) {
            return;
        }
        swapchain.SetHDR(enable);
        pp_settings.hdr = enable ? 1 : 0;
    }

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address);

    Frame* PrepareBlankFrame(bool present_thread);

    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);
    Frame* PrepareLastFrame();

private:
    Frame* GetRenderFrame();

    void RecreateFrame(Frame* frame, u32 width, u32 height);

    void SetExpectedGameSize(s32 width, s32 height);

private:
    float expected_ratio{1920.0 / 1080.0f};
    u32 expected_frame_width{1920};
    u32 expected_frame_height{1080};

    Frontend::WindowSDL& window;
    Instance instance;
    HostPasses::FsrPass fsr_pass;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass::Settings pp_settings{};
    HostPasses::PostProcessingPass pp_pass;
    AmdGpu::Liverpool* liverpool;
    Scheduler draw_scheduler;
    std::deque<u64> recent_frame_ticks; ///< wbp: BB_FRAMES_AHEAD bound (PrepareFrame)
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
    Swapchain swapchain;
    Runtime runtime;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    vk::UniqueCommandPool command_pool;
    std::vector<Frame> present_frames;
    std::queue<Frame*> free_queue;
    Frame* last_submit_frame;
    /// wbp: frame generation pacing: when the last game frame was shown, and the mean interval.
    std::chrono::steady_clock::time_point last_game_present{};
    /// wbp: frame generation: this frame without the HUD, through the same passes as the frame.
    Frame hudless_frame{};
    /// wbp: the game's curve after the HUD, as a table for the HUD-less copy: samples of the
    /// frame and of the copy without the table (two slots, read once the GPU is past them).
    bool SampleHudless(vk::CommandBuffer cmdbuf, vk::Image frame_image, u32 width, u32 height,
                       vk::Format format);
    std::unique_ptr<VideoCore::Buffer> hudless_samples[2];
    u64 hudless_sample_tick[2]{};
    u32 hudless_sample_slot = 0, hudless_sample_frame = 0;
    std::vector<float> hudless_histogram; ///< [channel][copy value][frame value]
    std::unique_ptr<VideoCore::Buffer> hudless_lut;
    /// The copy matched the frame outside the HUD in the last samples: only then is it handed
    /// to FSR 3 (a mismatch makes it take the whole screen for HUD and show the real frame).
    u32 hudless_good_samples = 0;
    bool hudless_matches = false;
    double game_interval_ms = 16.7;
    /// wbp: when an interpolated frame was last shown (steady clock ticks).
    static inline std::atomic<s64> frame_gen_shown{0};

public:
    /// wbp: frame generation showed a frame in the last quarter second (the frame limiter
    /// holds the game to half the refresh rate meanwhile).
    static bool FrameGenActive() {
        const s64 shown = frame_gen_shown.load(std::memory_order_relaxed);
        return shown != 0 &&
               std::chrono::steady_clock::now().time_since_epoch().count() - shown <
                   std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                       std::chrono::milliseconds(250))
                       .count();
    }

private:
    std::mutex free_mutex;
    std::condition_variable free_cv;
    std::condition_variable_any frame_cv;
    std::vector<VAddr> vo_buffers_addr;
};

} // namespace Vulkan
