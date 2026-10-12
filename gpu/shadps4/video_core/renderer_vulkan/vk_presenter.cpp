// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "wbp_bench.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "wbp_game_menu.h"
#include "wbp_overlay.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <vk_mem_alloc.h>
#ifdef MemoryBarrier
#undef MemoryBarrier // wbp: winnt.h macro (through fmt), clashes with vk::MemoryBarrier
#endif

namespace Vulkan {

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

// wbp: screenshot capture and ImGui overlays removed; the port presents directly.

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window}, runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    // wbp: two more frames for frame generation (an interpolated and a real one per flip).
    const u32 num_frames = num_images + 2;
    present_frames.resize(num_frames);
    for (u32 i = 0; i < num_frames; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    pp_pass.Create(device, swapchain.GetSurfaceFormat().format);
    // wbp: frame generation's table for the HUD-less copy (identity until fitted).
    hudless_lut = std::make_unique<VideoCore::Buffer>(instance, 0,
                                                      HostPasses::PostProcessingPass::LutBytes,
                                                      VideoCore::MemoryType::HostUncached,
                                                      "HUD-less curve table");
    {
        u32* lut = reinterpret_cast<u32*>(hudless_lut->mapped_data.data());
        for (u32 v = 0; v < 256; ++v) {
            lut[v] = v | v << 8 | v << 16;
        }
        hudless_lut->Flush(0, HostPasses::PostProcessingPass::LutBytes);
    }
    BbOverlay::Init(instance, swapchain.GetSurfaceFormat().format, num_images);

}

Presenter::~Presenter() {

    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
    if (hudless_frame.image) {
        vmaDestroyImage(instance.GetAllocator(), hudless_frame.image, hudless_frame.allocation);
        device.destroyImageView(hudless_frame.image_view);
    }
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

// wbp: raw copy of an image for BB_FRAME_DUMP_TRIGGER, written once the GPU is done.
static void DumpRaw(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                    vk::Image image, vk::ImageLayout layout, u32 w, u32 h, const char* name,
                    int index) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(w) * h * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return;
    }
    cmdbuf.copyImageToBuffer(image, layout, buffer,
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageExtent = {w, h, 1}});
    const char* dir = std::getenv("BB_DUMP_DIR");
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s_%03d_%ux%u.raw", dir && *dir ? dir : "out/dump", name,
                  index, w, h);
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      size = buffer_ci.size, file = std::string{path}] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(info.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    // wbp: BB_FRAME_DUMP_TRIGGER=<file>: when the file exists it is removed and this frame's
    // guest display buffer and presented image are written to BB_DUMP_DIR (default out/dump)
    // as raw 32-bit pixels (menus and movies included; the upscaler dumps scene frames only).
    static const char* frame_dump_trigger = std::getenv("BB_FRAME_DUMP_TRIGGER");
    static int frame_dump_index = 0;
    // BB_FRAME_DUMP_COUNT=N: N frames in a row from the trigger (frame generation checks: two
    // real frames and the interpolated one between them).
    static int frame_dump_left = 0;
    if (frame_dump_trigger && std::remove(frame_dump_trigger) == 0) {
        const char* count = std::getenv("BB_FRAME_DUMP_COUNT");
        frame_dump_left = std::max(count ? std::atoi(count) : 1, 1);
    }
    const bool frame_dump = frame_dump_left > 0;
    if (frame_dump) {
        --frame_dump_left;
    }
    // wbp: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    Frame* frame = GetRenderFrame();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    if (upscaled) {
        const auto device = instance.GetDevice();
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = view_info.format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = display.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    } else {
        auto& image = texture_cache.GetImage(image_id);
        if (frame_dump) {
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(),
                    vk::ImageLayout::eTransferSrcOptimal, image.info.size.width,
                    image.info.size.height, "display", frame_dump_index);
        }
        image_view = *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr);

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings, hudless_lut->Handle());
    if (frame_dump) {
        // The presented image as the swapchain blit reads it (General after the pass).
        const vk::MemoryBarrier2 done{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                      .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                      .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        DumpRaw(instance, draw_scheduler, cmdbuf, frame->image, vk::ImageLayout::eGeneral,
                frame->width, frame->height, "output", frame_dump_index);
        ++frame_dump_index;
    }

    // wbp: FSR 3 frame generation: the frame between the last one and this one, into a second
    // presenter frame that Present shows first.
    frame->generated = nullptr;
    if (auto& frame_gen = rasterizer->GetUpscaler().FrameGen(); frame_gen.Enabled()) {
        // The frame before the HUD, through the same passes, so that only the HUD differs.
        vk::Image hudless{};
        TemporalUpscaler::Hudless hud{};
        if (rasterizer->GetUpscaler().TakeHudless(hud) && FrameGeneration::UseHudless()) {
            const auto srgb = [](vk::Format f) {
                return f == vk::Format::eR8G8B8A8Unorm   ? vk::Format::eR8G8B8A8Srgb
                       : f == vk::Format::eB8G8R8A8Unorm ? vk::Format::eB8G8R8A8Srgb
                                                         : f;
            };
            const bool srgb_view = view_info.format == vk::Format::eR8G8B8A8Srgb ||
                                   view_info.format == vk::Format::eB8G8R8A8Srgb;
            const vk::Format hud_view_format =
                hud.display_buffer ? view_info.format : srgb_view ? srgb(hud.format) : hud.format;
            const auto device = instance.GetDevice();
            const vk::ImageView hud_view = Check(device.createImageView({
                .image = hud.image,
                .viewType = vk::ImageViewType::e2D,
                .format = hud_view_format,
                // The game's finished-frame target holds red and blue the other way round from
                // the display buffer (its display pass swaps them); measured equal otherwise.
                .components = hud.display_buffer
                                  ? vk::ComponentMapping{vk::ComponentSwizzle::eIdentity,
                                                         vk::ComponentSwizzle::eIdentity,
                                                         vk::ComponentSwizzle::eIdentity,
                                                         vk::ComponentSwizzle::eOne}
                                  : vk::ComponentMapping{vk::ComponentSwizzle::eB,
                                                         vk::ComponentSwizzle::eG,
                                                         vk::ComponentSwizzle::eR,
                                                         vk::ComponentSwizzle::eOne},
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            }));
            draw_scheduler.DeferOperation([device, hud_view] { device.destroyImageView(hud_view); });
            if (hudless_frame.width != frame->width || hudless_frame.height != frame->height ||
                hudless_frame.is_hdr != frame->is_hdr || !hudless_frame.image) {
                device.waitIdle(); // only when the window size changes
                RecreateFrame(&hudless_frame, frame->width, frame->height);
            }
            const vk::ImageMemoryBarrier2 to_target{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .image = hudless_frame.image,
                .subresourceRange = frame_subresources,
            };
            cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_target});
            const vk::Extent2D hud_size{hud.width, hud.height};
            const vk::ImageView passed = fsr_pass.Render(cmdbuf, hud_view, hud_size,
                                                         {frame->width, frame->height},
                                                         fsr_settings, frame->is_hdr);
            auto hud_pp = pp_settings;
            // Every 15 frames the copy is drawn without the table first and sampled.
            if (++hudless_sample_frame % 15 == 0) {
                pp_pass.Render(cmdbuf, passed, hud_size, hudless_frame, hud_pp,
                               hudless_lut->Handle());
                if (SampleHudless(cmdbuf, frame->image, frame->width, frame->height,
                                  swapchain.GetSurfaceFormat().format)) {
                    cmdbuf.pipelineBarrier2(
                        {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_target});
                }
            }
            hud_pp.use_lut = 1;
            pp_pass.Render(cmdbuf, passed, hud_size, hudless_frame, hud_pp, hudless_lut->Handle());
            hudless = hudless_frame.image;
        }
        // The copy is only used once it is known to match the frame outside the HUD: with a
        // brightness mismatch FSR 3 takes every pixel for HUD and the interpolated frame is
        // the real one (the counter doubled, the motion did not get smoother).
        const vk::Image hudless_rendered = hudless;
        if (!hudless_matches) {
            hudless = vk::Image{};
        }
        Frame* generated = GetRenderFrame();
        if (generated->width == frame->width && generated->height == frame->height &&
            frame_gen.Generate(cmdbuf, frame->image, swapchain.GetSurfaceFormat().format,
                               frame->width, frame->height, generated->image, hudless)) {
            frame->generated = generated;
        } else {
            std::scoped_lock fl{free_mutex};
            free_queue.push(generated);
            free_cv.notify_one();
        }
        if (frame->generated && frame_dump) {
            DumpRaw(instance, draw_scheduler, cmdbuf, generated->image, vk::ImageLayout::eGeneral,
                    frame->width, frame->height, "generated", frame_dump_index - 1);
            if (hudless_rendered) {
                DumpRaw(instance, draw_scheduler, cmdbuf, hudless_rendered,
                        vk::ImageLayout::eGeneral, frame->width, frame->height, "hudless",
                        frame_dump_index - 1);
            }
        }
    }

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    if (frame->generated) {
        frame->generated->ready_semaphore = frame->ready_semaphore;
        frame->generated->ready_tick = frame->ready_tick;
    }
    SubmitInfo info{};
    draw_scheduler.Flush(info);

    // wbp: the GPU command thread runs at most BB_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    static const u32 frames_ahead = [] {
        const char* env = std::getenv("BB_FRAMES_AHEAD");
        return env ? u32(std::max(0, std::atoi(env))) : 1u;
    }();
    if (frames_ahead) {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                draw_scheduler.Wait(tick);
            }
        }
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    // wbp: FSR 3 frame generation: the interpolated frame now, this one half a game frame later
    // (the mean interval between game frames).
    if (Frame* generated = std::exchange(frame->generated, nullptr)) {
        // Both images are finished by the same submission. When the GPU is the limit, the
        // presents were queued long before it got there: both images became ready at once and
        // the real frame replaced the interpolated one (mailbox), so the counter rose and the
        // picture did not get smoother. The spacing is therefore counted from when the GPU has
        // finished them.
        const vk::SemaphoreWaitInfo ready_info{
            .semaphoreCount = 1,
            .pSemaphores = &frame->ready_semaphore,
            .pValues = &frame->ready_tick,
        };
        while (instance.GetDevice().waitSemaphores(ready_info, 1'000'000'000ull) ==
               vk::Result::eTimeout) {
        }
        // Counted from when the interpolated frame was queued (its acquire can block for a few
        // ms): both frames then get the same spacing on screen.
        Present(generated, false, false);
        const auto shown = std::chrono::steady_clock::now();
        frame_gen_shown.store(shown.time_since_epoch().count(), std::memory_order_relaxed);
        // Half a game frame, and about one display refresh at least: the game runs at half the
        // refresh rate meanwhile (VideoOutDriver), so both frames get the same time on screen
        // whether the driver keeps FIFO or forces V-Sync off.
        const double refresh_ms = 1000.0 / double(BbDisplayRefreshHz());
        const double hold_ms = std::max(game_interval_ms * 0.5, refresh_ms - 0.25);
        std::this_thread::sleep_until(
            shown + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double, std::milli>(hold_ms)));
    }
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // wbp: a minimized window has no pixels on Windows (0x0); keep the swapchain and skip
    // presenting until the window is restored.
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        free_frame();
        return;
    }

    // wbp: frame generation presents through FIFO (see Swapchain::SetFrameGeneration).
    swapchain.SetFrameGeneration(rasterizer->GetUpscaler().FrameGen().Enabled());

    // Recreate the swapchain if the window was resized.
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            free_frame();
            return;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    // wbp: the game frame is blitted (letterboxed) straight into the swapchain image.
    const vk::Image swapchain_image = swapchain.Image();
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
        cmdbuf.clearColorImage(swapchain_image, vk::ImageLayout::eTransferDstOptimal, black, color_range);
        const vk::MemoryBarrier clear_done{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
        cmdbuf.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height),
                         vk::Filter::eLinear);
        BbGameMenu::Poll(); // the port's pages in the game's System menu
        // wbp: the settings menu / FPS counter over the frame, at display resolution.
        const bool overlay = BbOverlay::Visible();
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        if (overlay) {
            BbOverlay::Render(cmdbuf, swapchain.ImageView(), extent);
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = vk::ImageLayout::ePresentSrcKHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);

    // Present to swapchain.
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        if (!swapchain.Present() && window.GetWidth() != 0 && window.GetHeight() != 0) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }

    free_frame();
    BbStats::presents.fetch_add(1, std::memory_order_relaxed);
    BbBench::OnPresent();
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
        const auto now = std::chrono::steady_clock::now();
        if (last_game_present.time_since_epoch().count() != 0) {
            const double ms =
                std::chrono::duration<double, std::milli>(now - last_game_present).count();
            game_interval_ms = std::clamp(game_interval_ms * 0.9 + ms * 0.1, 4.0, 50.0);
        }
        last_game_present = now;
    }
}

// The game's final pass maps every encoded value through a curve after the HUD (its brightness
// setting; measured exact per value and channel); the HUD-less copy is taken before it. Every
// 15 frames a block of the frame and of the copy drawn without the table is read back; the
// value pairs fill a histogram per channel and each value maps to the most frequent result. FSR 3
// takes every pixel that differs between the copy and the frame for HUD (the darkest by a single
// step), so a fitted power curve was not close enough: the whole frame came out as the real one.
bool Presenter::SampleHudless(vk::CommandBuffer cmdbuf, vk::Image frame_image, u32 width,
                              u32 height, vk::Format format) {
    constexpr u32 Side = 256;
    if (width < 2 * Side || height < 2 * Side) {
        return false;
    }
    constexpr u64 Bytes = u64(Side) * Side * 4;
    const u32 slot = hudless_sample_slot;
    hudless_sample_slot ^= 1;
    auto& buffer = hudless_samples[slot];
    if (buffer && hudless_sample_tick[slot] != 0 &&
        draw_scheduler.GetWorkSemaphore()->IsFree(hudless_sample_tick[slot])) {
        buffer->Invalidate(0, 2 * Bytes);
        const u8* frame_px = buffer->mapped_data.data();
        const u8* copy_px = frame_px + Bytes;
        if (hudless_histogram.empty()) {
            hudless_histogram.assign(3 * 256 * 256, 0.0f);
        }
        for (float& count : hudless_histogram) {
            count *= 0.5f; // follows a change of the game's brightness setting
        }
        for (u64 i = 0; i < Bytes; i += 4) {
            for (u32 c = 0; c < 3; ++c) {
                hudless_histogram[(c * 256 + copy_px[i + c]) * 256 + frame_px[i + c]] += 1.0f;
            }
        }
        // Per memory channel: the most frequent result, values never seen interpolated.
        std::array<std::array<u8, 256>, 3> table{};
        for (u32 c = 0; c < 3; ++c) {
            std::array<int, 256> best{};
            for (u32 v = 0; v < 256; ++v) {
                const float* row = &hudless_histogram[(c * 256 + v) * 256];
                const auto top = std::max_element(row, row + 256);
                best[v] = *top >= 1.0f ? int(top - row) : -1;
            }
            int last = -1;
            for (int v = 0; v < 256; ++v) {
                if (best[v] < 0) {
                    continue;
                }
                for (int u = last + 1; u < v; ++u) {
                    table[c][u] = last < 0 ? u8(best[v])
                                           : u8(std::lround(best[last] + float(best[v] - best[last]) *
                                                                             float(u - last) /
                                                                             float(v - last)));
                }
                table[c][v] = u8(best[v]);
                last = v;
            }
            for (int u = last + 1; u < 256; ++u) {
                table[c][u] = last < 0 ? u8(u) : u8(best[last]);
            }
        }
        // How much of this sample the table reproduces exactly (the rest is HUD or subtitles).
        u64 exact = 0;
        for (u64 i = 0; i < Bytes; i += 4) {
            exact += table[0][copy_px[i]] == frame_px[i] && table[1][copy_px[i + 1]] == frame_px[i + 1] &&
                     table[2][copy_px[i + 2]] == frame_px[i + 2];
        }
        const double share = double(exact) / double(Side * Side);
        // The shader reads R, G, B of the view: memory order B, G, R for the BGRA formats.
        const bool bgra = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
        const u32 r = bgra ? 2 : 0, b = bgra ? 0 : 2;
        u32* lut = reinterpret_cast<u32*>(hudless_lut->mapped_data.data());
        for (u32 v = 0; v < 256; ++v) {
            lut[v] = u32(table[r][v]) | u32(table[1][v]) << 8 | u32(table[b][v]) << 16;
        }
        hudless_lut->Flush(0, HostPasses::PostProcessingPass::LutBytes);
        // Two good samples in a row switch the copy on, one bad one off.
        const bool good = share > 0.9;
        hudless_good_samples = good ? hudless_good_samples + 1 : 0;
        const bool matches = hudless_good_samples >= 2;
        if (matches != hudless_matches) {
            std::printf("Frame generation: HUD-less copy %s (%.1f%% exact)\n",
                        matches ? "matches the frame; interpolating without the HUD"
                                : "differs from the frame; interpolating the whole frame",
                        share * 100.0);
            hudless_matches = matches;
        }
        hudless_sample_tick[slot] = 0;
    }
    if (!buffer) {
        buffer = std::make_unique<VideoCore::Buffer>(instance, 0, 2 * Bytes,
                                                     VideoCore::MemoryType::HostCached,
                                                     "HUD-less curve samples");
    }
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    const vk::Offset3D at{s32(width / 2 - Side / 2), s32(height / 2 - Side / 4), 0};
    for (u32 i = 0; i < 2; ++i) {
        const vk::BufferImageCopy region{
            .bufferOffset = i * Bytes,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageOffset = at,
            .imageExtent = {Side, Side, 1},
        };
        cmdbuf.copyImageToBuffer(i == 0 ? frame_image : hudless_frame.image,
                                 vk::ImageLayout::eGeneral, buffer->Handle(), region);
    }
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
                         vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    hudless_sample_tick[slot] = draw_scheduler.CurrentTick();
    return true;
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // no surface (minimized window): keep the last frame size
    }
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
