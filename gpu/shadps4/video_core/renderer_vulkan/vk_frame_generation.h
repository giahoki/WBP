// SPDX-License-Identifier: GPL-2.0-or-later
// wbp: FSR 3 frame generation (frame_generation=1 in wbp.ini, BB_FRAME_GEN=0/1). Between two
// presented game frames one more is interpolated by FSR 3 Frame Interpolation and Optical Flow
// (FireBurn/FSR-Vulkan, the same portable library as the FSR 3.1 upscaler): the displayed frame
// rate nearly doubles at the cost of about half a frame of extra latency.
//
// The upscaler hands over this frame's depth and motion vectors right after its own dispatch
// (copied, the game reuses its targets), the presenter asks for the interpolated frame once the
// final frame (with the HUD) is composed, and shows it before the real one.

#pragma once

#include <array>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

struct FfxVkPortableFrameGenerationContext;

namespace Vulkan {

class Instance;

class FrameGeneration {
public:
    explicit FrameGeneration(const Instance& instance);
    ~FrameGeneration();
    FrameGeneration(const FrameGeneration&) = delete;
    FrameGeneration& operator=(const FrameGeneration&) = delete;

    /// The setting is on and the device can run FSR 3 frame interpolation.
    [[nodiscard]] bool Enabled() const;
    /// Interpolate the frame before the HUD (BB_FG_HUDLESS=0: the frame with its HUD, which then
    /// moves with the scene). The game's final pass applies its brightness curve after the HUD:
    /// the presenter fits that curve to the copy (else FSR 3 takes the whole screen for HUD and
    /// every interpolated frame is the current one).
    [[nodiscard]] static bool UseHudless();

    /// Upscaler, after its dispatch: this frame's depth (`depth_format`, depth aspect) and the
    /// R16G16 motion vectors, both in General layout at the render size `width` x `height`.
    void Capture(vk::CommandBuffer cmdbuf, vk::Image depth, vk::Format depth_format,
                 vk::Image motion, u32 width, u32 height, std::array<float, 2> jitter,
                 float camera_near, float camera_fov, float frame_ms, bool reset);

    /// Presenter: interpolates between the previous and this final frame (`frame_image`,
    /// General layout, `frame_format`) into `generated_image` (left in General). With
    /// `hudless_image` (the same frame before the HUD, same format and size, General) the scene
    /// is interpolated without the HUD and the HUD comes from this frame. False when there is
    /// nothing to interpolate this frame (menus, the first frame, HDR, an error).
    bool Generate(vk::CommandBuffer cmdbuf, vk::Image frame_image, vk::Format frame_format,
                  u32 width, u32 height, vk::Image generated_image, vk::Image hudless_image);

private:
    bool EnsureContext(u32 width, u32 height, vk::Format source_format);
    void DestroyContext();

    const Instance& instance;
    FfxVkPortableFrameGenerationContext* context = nullptr;
    bool unsupported = false;
    bool failed = false;

    // Copies of the upscaler's inputs, valid until the presenter uses them.
    VideoCore::UniqueImage depth_copy;
    VideoCore::UniqueImage motion_copy;
    vk::Format depth_copy_format{};
    u32 render_width = 0, render_height = 0;
    bool captured = false;
    bool capture_reset = true;
    std::array<float, 2> capture_jitter{};
    float capture_near = 0.1f, capture_fov = 1.0f, capture_frame_ms = 16.6f;

    // The interpolated frame, before it is copied into the presenter's frame.
    VideoCore::UniqueImage output;
    vk::Format output_format{};
    u32 display_width = 0, display_height = 0;
    vk::Format source_format{};
    u32 context_render_width = 0, context_render_height = 0;
    bool reset = true;
    bool last_hudless = false; ///< the source changes: the history restarts
    u64 frame_id = 0;
};

} // namespace Vulkan
