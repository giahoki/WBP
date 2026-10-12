// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_frame_generation.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "ffx_vk_portable.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "wbp_settings.h"

namespace Vulkan {
namespace {

FfxVkPortableImage Describe(vk::Image image, vk::Format format, u32 width, u32 height,
                            vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                            FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = image;
    out.format = static_cast<VkFormat>(format);
    out.extent = {width, height};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = static_cast<VkImageUsageFlags>(usage);
    out.aspect = static_cast<VkImageAspectFlags>(aspect);
    out.state = state;
    return out;
}

FfxVkPortableImage Absent() {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    return out;
}

/// The presenter's frames use the swapchain format; frame interpolation reads and writes the
/// encoded values, so sRGB formats are viewed as UNORM (the frames are created mutable).
vk::Format SourceFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eB8G8R8A8Srgb:
    case vk::Format::eB8G8R8A8Unorm:
        return vk::Format::eB8G8R8A8Unorm;
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eR8G8B8A8Unorm:
        return vk::Format::eR8G8B8A8Unorm;
    case vk::Format::eA8B8G8R8SrgbPack32:
    case vk::Format::eA8B8G8R8UnormPack32:
        return vk::Format::eA8B8G8R8UnormPack32;
    default:
        return vk::Format::eUndefined; // HDR swapchains: not interpolated
    }
}

void MemoryBarrier(vk::CommandBuffer cmdbuf, vk::PipelineStageFlags2 src_stage,
                   vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                   vk::AccessFlags2 dst_access) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
}

void ImageBarrier(vk::CommandBuffer cmdbuf, vk::Image image, vk::ImageAspectFlags aspect,
                  vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                  vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                  vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
}

constexpr auto AllCommands = vk::PipelineStageFlagBits2::eAllCommands;
constexpr auto AnyAccess = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;

} // namespace

FrameGeneration::FrameGeneration(const Instance& instance_) : instance{instance_} {
    FfxVkPortableDeviceCapabilities caps{};
    caps.structSize = sizeof(caps);
    if (ffxVkPortableQueryDeviceCapabilities(instance.GetPhysicalDevice(), &caps) !=
            FFX_VK_PORTABLE_OK ||
        !caps.fsr3FrameGenerationPrerequisites) {
        unsupported = true;
    }
    std::printf("Frame generation: FSR 3 %s; %s\n", unsupported ? "unsupported" : "available",
                BbSettings::Get().frame_generation ? "on" : "off");
}

FrameGeneration::~FrameGeneration() {
    DestroyContext();
}

bool FrameGeneration::Enabled() const {
    return !unsupported && !failed && BbSettings::Get().frame_generation;
}

void FrameGeneration::DestroyContext() {
    if (context) {
        instance.GetDevice().waitIdle();
        ffxVkPortableFrameGenerationContextDestroy(context);
        context = nullptr;
    }
}

void FrameGeneration::Capture(vk::CommandBuffer cmdbuf, vk::Image depth, vk::Format depth_format,
                              vk::Image motion, u32 width, u32 height,
                              std::array<float, 2> jitter, float camera_near, float camera_fov,
                              float frame_ms, bool reset_history) {
    if (!Enabled()) {
        return;
    }
    const auto device = instance.GetDevice();
    if (!depth_copy || width != render_width || height != render_height ||
        depth_format != depth_copy_format) {
        if (depth_copy) {
            device.waitIdle();
        }
        const auto allocator = instance.GetAllocator();
        depth_copy = VideoCore::UniqueImage(device, allocator);
        depth_copy.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = depth_format,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        });
        motion_copy = VideoCore::UniqueImage(device, allocator);
        motion_copy.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR16G16Sfloat,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                     vk::ImageUsageFlagBits::eTransferDst,
        });
        ImageBarrier(cmdbuf, vk::Image(depth_copy), vk::ImageAspectFlagBits::eDepth,
                     vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, AllCommands,
                     vk::AccessFlagBits2::eNone, AllCommands, AnyAccess);
        ImageBarrier(cmdbuf, vk::Image(motion_copy), vk::ImageAspectFlagBits::eColor,
                     vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, AllCommands,
                     vk::AccessFlagBits2::eNone, AllCommands, AnyAccess);
        render_width = width;
        render_height = height;
        depth_copy_format = depth_format;
        capture_reset = true;
    }
    // The upscaler read both; the interpolation of the last frame read the copies.
    MemoryBarrier(cmdbuf, AllCommands, AnyAccess, vk::PipelineStageFlagBits2::eCopy,
                  vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite);
    const vk::ImageCopy depth_region{
        .srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
        .extent = {width, height, 1},
    };
    cmdbuf.copyImage(depth, vk::ImageLayout::eGeneral, vk::Image(depth_copy),
                     vk::ImageLayout::eGeneral, depth_region);
    const vk::ImageCopy motion_region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {width, height, 1},
    };
    cmdbuf.copyImage(motion, vk::ImageLayout::eGeneral, vk::Image(motion_copy),
                     vk::ImageLayout::eGeneral, motion_region);
    MemoryBarrier(cmdbuf, vk::PipelineStageFlagBits2::eCopy,
                  vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
                  AllCommands, AnyAccess);
    captured = true;
    capture_reset = capture_reset || reset_history;
    capture_jitter = jitter;
    capture_near = camera_near;
    capture_fov = camera_fov;
    capture_frame_ms = frame_ms;
}

bool FrameGeneration::UseHudless() {
    static const bool use = [] {
        const char* env = std::getenv("BB_FG_HUDLESS");
        return !(env && env[0] == '0');
    }();
    return use;
}

bool FrameGeneration::EnsureContext(u32 width, u32 height, vk::Format format) {
    if (context && width == display_width && height == display_height && format == source_format &&
        render_width == context_render_width && render_height == context_render_height) {
        return true;
    }
    DestroyContext();
    const auto device = instance.GetDevice();
    const auto features = instance.GetPhysicalDevice().getFormatProperties(format);
    if (!(features.optimalTilingFeatures & vk::FormatFeatureFlagBits::eStorageImage)) {
        std::printf("Frame generation: %s has no storage support; off\n",
                    vk::to_string(format).c_str());
        failed = true;
        return false;
    }

    FfxVkPortableDeviceInfo device_info{};
    device_info.structSize = sizeof(device_info);
    device_info.instance = instance.GetInstance();
    device_info.physicalDevice = instance.GetPhysicalDevice();
    device_info.device = device;
    device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    device_info.queue = instance.GetGraphicsQueue();
    device_info.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
    device_info.shaderFloat16Enabled = instance.IsShaderFloat16Enabled();
    device_info.subgroupSizeControlEnabled = instance.IsSubgroupSizeControlEnabled();
    device_info.synchronization2Enabled = VK_TRUE;
    device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;

    FfxVkPortableFrameGenerationCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    create_info.maxRenderSize = {render_width, render_height};
    create_info.displaySize = {width, height};
    create_info.interpolationSourceFormat = static_cast<VkFormat>(format);
    create_info.outputFormat = static_cast<VkFormat>(format);
    if (const u64 issues = ffxVkPortableValidateFrameGenerationCreateInfo(&create_info)) {
        std::printf("Frame generation: invalid create info (issues %#llx); off\n",
                    static_cast<unsigned long long>(issues));
        failed = true;
        return false;
    }
    if (ffxVkPortableFrameGenerationContextCreate(&device_info, &create_info, &context) !=
        FFX_VK_PORTABLE_OK) {
        std::printf("Frame generation: FSR 3 context creation failed; off\n");
        context = nullptr;
        failed = true;
        return false;
    }

    output = VideoCore::UniqueImage(device, instance.GetAllocator());
    output.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
    });
    output_format = format;
    display_width = width;
    display_height = height;
    source_format = format;
    context_render_width = render_width;
    context_render_height = render_height;
    reset = true;
    std::printf("Frame generation: FSR 3 context %ux%u -> %ux%u (%s)\n", render_width,
                render_height, width, height, vk::to_string(format).c_str());
    return true;
}

bool FrameGeneration::Generate(vk::CommandBuffer cmdbuf, vk::Image frame_image,
                               vk::Format frame_format, u32 width, u32 height,
                               vk::Image generated_image, vk::Image hudless_image) {
    if (!Enabled() || !captured) {
        // Menus and loading screens: nothing to interpolate; restart the history afterwards.
        reset = true;
        captured = false;
        return false;
    }
    captured = false;
    const vk::Format format = SourceFormat(frame_format);
    if (format == vk::Format::eUndefined) {
        reset = true;
        return false;
    }
    const bool new_output = !output || width != display_width || height != display_height ||
                            format != source_format || render_width != context_render_width ||
                            render_height != context_render_height;
    if (!EnsureContext(width, height, format)) {
        return false;
    }
    if (new_output) {
        ImageBarrier(cmdbuf, vk::Image(output), vk::ImageAspectFlagBits::eColor,
                     vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, AllCommands,
                     vk::AccessFlagBits2::eNone, AllCommands, AnyAccess);
    }
    // The final frame was just written (post pass); the output was copied last frame.
    MemoryBarrier(cmdbuf, AllCommands, AnyAccess, AllCommands, AnyAccess);

    // Optical flow and the previous interpolation source must come from the same kind of image.
    if (bool(hudless_image) != last_hudless) {
        last_hudless = bool(hudless_image);
        reset = true;
    }
    const bool restart = reset || capture_reset;
    {
        // How often the history restarts (an interpolated frame after a restart is the current
        // frame itself): printed every 2 s.
        static u32 frames = 0, resets = 0, own_resets = 0;
        static auto printed = std::chrono::steady_clock::now();
        ++frames;
        resets += restart ? 1 : 0;
        own_resets += reset ? 1 : 0;
        if (std::chrono::steady_clock::now() - printed >= std::chrono::seconds(2)) {
            printed = std::chrono::steady_clock::now();
            std::printf("Frame generation: %u frames, %u restarts (%u own, %u from the upscaler)\n",
                        frames, resets, own_resets, resets - own_resets);
            frames = resets = own_resets = 0;
        }
    }
    const vk::ImageUsageFlags frame_usage =
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled;
    const FfxVkPortableImage current =
        Describe(frame_image, format, width, height, frame_usage, vk::ImageAspectFlagBits::eColor,
                 FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    const FfxVkPortableImage hudless =
        hudless_image ? Describe(hudless_image, format, width, height, frame_usage,
                                 vk::ImageAspectFlagBits::eColor,
                                 FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ)
                      : Absent();
    // Optical flow and the interpolation must see the same image: the HUD-less one if any.
    const FfxVkPortableImage& source = hudless_image ? hudless : current;

    FfxVkPortableFrameGenerationPrepareInfo prepare{};
    prepare.structSize = sizeof(prepare);
    prepare.commandBuffer = cmdbuf;
    prepare.depth = Describe(vk::Image(depth_copy), depth_copy_format, render_width,
                             render_height,
                             vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                             vk::ImageAspectFlagBits::eDepth,
                             FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    prepare.motionVectors = Describe(
        vk::Image(motion_copy), vk::Format::eR16G16Sfloat, render_width, render_height,
        vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
            vk::ImageUsageFlagBits::eTransferDst,
        vk::ImageAspectFlagBits::eColor, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    prepare.renderSize = {render_width, render_height};
    prepare.jitterOffset = {capture_jitter[0], capture_jitter[1]};
    prepare.motionVectorScale = {1.0f, 1.0f};
    prepare.frameTimeMilliseconds = capture_frame_ms;
    prepare.cameraNear = capture_near;
    prepare.cameraFar = 3000.0f;
    prepare.cameraVerticalFovRadians = capture_fov;
    prepare.viewSpaceToMeters = 1.0f;
    prepare.minLuminance = 0.0f;
    prepare.maxLuminance = 1.0f;
    prepare.transferFunction = FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB;
    prepare.cameraUp = {0.0f, 1.0f, 0.0f};
    prepare.cameraRight = {1.0f, 0.0f, 0.0f};
    prepare.cameraForward = {0.0f, 0.0f, 1.0f};
    prepare.reset = restart ? VK_TRUE : VK_FALSE;
    prepare.frameId = frame_id;

    FfxVkPortableFrameGenerationDispatchInfo dispatch{};
    dispatch.structSize = sizeof(dispatch);
    dispatch.commandBuffer = cmdbuf;
    dispatch.currentColor = current;
    dispatch.hudlessColor = hudless;
    dispatch.distortionField = Absent();
    dispatch.output = Describe(vk::Image(output), output_format, width, height,
                               vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                                   vk::ImageUsageFlagBits::eTransferSrc,
                               vk::ImageAspectFlagBits::eColor,
                               FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.displaySize = {width, height};
    dispatch.interpolationRect = {0, 0, width, height};
    dispatch.frameTimeMilliseconds = capture_frame_ms;
    dispatch.cameraNear = capture_near;
    dispatch.cameraFar = 3000.0f;
    dispatch.cameraVerticalFovRadians = capture_fov;
    dispatch.viewSpaceToMeters = 1.0f;
    dispatch.minLuminance = 0.0f;
    dispatch.maxLuminance = 1.0f;
    dispatch.transferFunction = FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB;
    dispatch.reset = prepare.reset;
    dispatch.frameId = frame_id;

    FfxVkPortableFrameGenerationCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    create_info.maxRenderSize = {render_width, render_height};
    create_info.displaySize = {width, height};
    create_info.interpolationSourceFormat = static_cast<VkFormat>(format);
    create_info.outputFormat = static_cast<VkFormat>(format);
    if (const u64 issues = ffxVkPortableValidateFrameGenerationPrepareInfo(&create_info, &prepare)) {
        std::printf("Frame generation: invalid prepare info (issues %#llx); off\n",
                    static_cast<unsigned long long>(issues));
        failed = true;
        return false;
    }
    if (const u64 issues =
            ffxVkPortableValidateFrameGenerationDispatchInfo(&create_info, &dispatch)) {
        std::printf("Frame generation: invalid dispatch info (issues %#llx); off\n",
                    static_cast<unsigned long long>(issues));
        failed = true;
        return false;
    }
    if (ffxVkPortableFrameGenerationContextPrepare(context, &prepare, &source) !=
            FFX_VK_PORTABLE_OK ||
        ffxVkPortableFrameGenerationContextRecordDispatch(context, &dispatch) !=
            FFX_VK_PORTABLE_OK) {
        std::printf("Frame generation: FSR 3 dispatch failed; off\n");
        failed = true;
        return false;
    }
    ++frame_id;
    reset = false;
    capture_reset = false;

    // The interpolated frame into the presenter's frame image (same texel size, sRGB view).
    ImageBarrier(cmdbuf, vk::Image(output), vk::ImageAspectFlagBits::eColor,
                 vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral, AllCommands, AnyAccess,
                 vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
    ImageBarrier(cmdbuf, generated_image, vk::ImageAspectFlagBits::eColor,
                 vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, AllCommands,
                 vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eCopy,
                 vk::AccessFlagBits2::eTransferWrite);
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {width, height, 1},
    };
    cmdbuf.copyImage(vk::Image(output), vk::ImageLayout::eGeneral, generated_image,
                     vk::ImageLayout::eTransferDstOptimal, region);
    ImageBarrier(cmdbuf, generated_image, vk::ImageAspectFlagBits::eColor,
                 vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral,
                 vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
                 AllCommands, AnyAccess);
    return true;
}

} // namespace Vulkan
