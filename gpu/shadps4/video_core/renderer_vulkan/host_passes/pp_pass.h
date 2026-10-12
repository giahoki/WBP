//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Frame;
}

namespace Vulkan::HostPasses {

class PostProcessingPass {
public:
    struct Settings {
        float gamma = 1.0f;
        u32 hdr = 0;
        u32 srgb_input = 0;
        float power = 1.0f; ///< wbp: power curve on the encoded result (1: none)
        u32 use_lut = 0;    ///< wbp: map the encoded result through the `lut` buffer
    };

    void Create(vk::Device device, vk::Format surface_format);

    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings, vk::Buffer lut);

    /// wbp: size of the `lut` buffer: 256 entries of R | G << 8 | B << 16.
    static constexpr u64 LutBytes = 256 * 4;

private:
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
};

} // namespace Vulkan::HostPasses
