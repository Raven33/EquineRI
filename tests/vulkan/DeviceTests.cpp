// Vulkan-only GPU tests: GPU listing and device creation, native device
// additions (features, extensions, pNext), layout modes and resting layouts,
// shader printf, native sections and import, and work graphs reporting
// unsupported. Portable tests live in tests/gpu/.

#include <TestHarness.hpp>

#include "../gpu/GpuTestCommon.hpp"
#include "../gpu/TextureChecks.hpp"

#include <RI/RI.hpp>
#include <RI/Vk/Native.hpp>
#include <RI/Helpers/TypedCommands.hpp>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdlib>
#include <vector>

EQ_TEST(vk, list_gpus) {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    EQ_CHECK(!gpus.empty());
    for (const RI::Gpu &gpu : gpus) {
        EQ_CHECK(!gpu.name.empty());
        EQ_CHECK(gpu.vendorId != 0);
        EQ_CHECK(gpu.memory > 0);
        EQ_CHECK((gpu.id != std::array<uint8, 16>{}));
    }
}

EQ_TEST(vk, create_destroy_device) {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    RI::Device device = RI::createDevice({.gpu = gpus[0]});
    EQ_CHECK(device.gpu.id == gpus[0].id);
    EQ_CHECK(device.gpu.name == gpus[0].name);

    const RI::Vk::NativeDevice n = RI::Vk::native(device);
    EQ_CHECK(n.instance != VK_NULL_HANDLE);
    EQ_CHECK(n.device != VK_NULL_HANDLE);
    EQ_CHECK(n.graphicsQueue != VK_NULL_HANDLE);
    EQ_CHECK(n.apiVersion >= VK_API_VERSION_1_3);
    EQ_CHECK(n.functions != nullptr && n.functions->vkDeviceWaitIdle != nullptr);
    EQ_CHECK(n.functions->vkDeviceWaitIdle(n.device) == VK_SUCCESS);

    RI::destroyDevice(device);
    EQ_CHECK(device.internal.state == nullptr);
}

// Two devices at once, each with its own instance (P92).
EQ_TEST(vk, two_devices) {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    RI::Device a = RI::createDevice({.gpu = gpus[0]});
    RI::Device b = RI::createDevice({.gpu = gpus[0]});
    EQ_CHECK(RI::Vk::native(a).instance != RI::Vk::native(b).instance);
    RI::destroyDevice(a);
    EQ_CHECK(RI::Vk::native(b).functions->vkDeviceWaitIdle(RI::Vk::native(b).device) == VK_SUCCESS);
    RI::destroyDevice(b);
}

// Robustness, off by default, turned on by editing RI's features (P92).
EQ_TEST(vk, native_features_turn_on_robustness) {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    RI::Vk::DeviceFeatures features = RI::Vk::defaultDeviceFeatures(gpus[0]);
    EQ_CHECK(features.core.robustBufferAccess == VK_FALSE);
    EQ_CHECK(features.v12.bufferDeviceAddress == VK_TRUE);
    features.core.robustBufferAccess = VK_TRUE;
    features.v13.robustImageAccess = VK_TRUE;
    features.v14.pipelineRobustness = VK_TRUE;
    RI::Device device = RI::Vk::createDevice({.gpu = gpus[0]}, {.features = &features});
    EQ_CHECK(RI::Vk::native(device).device != VK_NULL_HANDLE);
    RI::destroyDevice(device);
}

// An extension RI does not use, with its feature structure appended after RI's.
EQ_TEST(vk, native_extension_and_pnext) {
    const std::vector<RI::Gpu> gpus = RI::listGpus();
    const char *const extensions[] = {VK_KHR_SHADER_CLOCK_EXTENSION_NAME};
    VkPhysicalDeviceShaderClockFeaturesKHR clock{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR,
                                                 .shaderSubgroupClock = VK_TRUE};
    RI::Device device = RI::Vk::createDevice({.gpu = gpus[0]}, {.extensions = extensions, .pNext = &clock});
    EQ_CHECK(RI::Vk::native(device).device != VK_NULL_HANDLE);
    EQ_CHECK(clock.pNext == nullptr); // left untouched
    RI::destroyDevice(device);
}

// Listing with a device alive must not break the device's instance functions.
EQ_TEST(vk, list_gpus_while_device_alive) {
    RI::Device device = RI::createDevice({.gpu = RI::listGpus()[0]});
    EQ_CHECK(!RI::listGpus().empty());
    RI::Vk::defaultDeviceFeatures(device.gpu);
    RI::destroyDevice(device);
    EQ_CHECK(device.internal.state == nullptr);
}

// A broken driver manifest makes the loader report an error (CMake points
// VK_ADD_DRIVER_FILES at one). It is not the program's fault: no abort.
// Linux only: the Windows loader fails the whole instance instead (see
// tests/CMakeLists.txt).
#ifndef _WIN32
EQ_TEST(vk, loader_errors_do_not_abort) {
    RI::Device device = RI::createDevice({.gpu = RI::listGpus()[0]});
    RI::destroyDevice(device);
}
#endif

// Aborts by design (see tests/CMakeLists.txt).
EQ_TEST(vk, unknown_gpu_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    RI::Gpu missing;
    missing.name = "Nonexistent GPU";
    RI::createDevice({.gpu = missing});
}

// Mode T resting layouts of sampled depth (P97): fast sampling by default,
// the depth read-only layout with eReadOnlyDepthSampling.
EQ_TEST(vk, sampled_depth_resting_layout) {
    RI::Device device = RI::Vk::createDevice({.gpu = gputest::pickGpu()}, {.layoutMode = RI::Vk::LayoutMode::eTransitions});
    gputest::Arena arena{.memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 8ull << 20)};
    const RI::TextureUsage usage = RI::TextureUsage::eSampled | RI::TextureUsage::eDepthStencil;
    RI::Texture fast = gputest::placeTexture(device, arena, {.width = 64, .height = 64, .format = RI::Format::eD32Float, .usage = usage});
    RI::Texture readOnly = gputest::placeTexture(
        device, arena,
        {.width = 64, .height = 64, .format = RI::Format::eD32Float, .usage = usage | RI::TextureUsage::eReadOnlyDepthSampling});
    RI::Texture color = gputest::placeTexture(
        device, arena, {.width = 64, .height = 64, .format = RI::Format::eRGBA8Unorm, .usage = RI::TextureUsage::eSampled});
    EQ_CHECK(RI::Vk::native(fast).restingLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EQ_CHECK(RI::Vk::native(readOnly).restingLayout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
    EQ_CHECK(RI::Vk::native(color).restingLayout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
    RI::destroyTexture(device, color);
    RI::destroyTexture(device, readOnly);
    RI::destroyTexture(device, fast);
    RI::freeMemory(device, arena.memory);
    RI::destroyDevice(device);
}

// Every layout mode the GPU supports gives the same results (L1, P78, P95):
// render -> sample, depth -> sample and storage -> sample.
EQ_TEST(vk, layout_modes) {
    const RI::Gpu gpu = gputest::pickGpu();
    RI::Device automatic = RI::createDevice({.gpu = gpu});
    const RI::Vk::LayoutMode chosen = RI::Vk::native(automatic).layoutMode;
    EQ_CHECK(chosen != RI::Vk::LayoutMode::eAuto);
    RI::destroyDevice(automatic);

    bool unified = false;
#ifdef VK_KHR_unified_image_layouts
    unified = RI::Vk::defaultDeviceFeatures(gpu).unifiedLayouts.unifiedImageLayouts == VK_TRUE;
#endif
    EQ_CHECK(chosen == (unified ? RI::Vk::LayoutMode::eUnified : RI::Vk::LayoutMode::eTransitions));
    std::vector<RI::Vk::LayoutMode> modes{RI::Vk::LayoutMode::eGeneral, RI::Vk::LayoutMode::eTransitions};
    if (unified) {
        modes.push_back(RI::Vk::LayoutMode::eUnified);
    }
    for (RI::Vk::LayoutMode mode : modes) {
        RI::Device device = RI::Vk::createDevice({.gpu = gpu}, {.layoutMode = mode});
        EQ_CHECK(RI::Vk::native(device).layoutMode == mode);
        gputest::renderThenSample(device);
        gputest::depthWriteThenSample(device);
        gputest::storageWriteThenSample(device);
        RI::destroyDevice(device);
    }
}

#if EQUINE_VALIDATION
// Shader printf (P53 (3)): the validation layer prints it to stdout; ctest
// checks the text.
EQ_TEST(vk, shader_printf) {
    RI::Device device = RI::createDevice({.gpu = gputest::pickGpu()});
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 8});
    const std::vector<std::byte> shader = gputest::loadShader("printf");
    RI::Pipeline pipeline = RI::createComputePipeline(device, {.shader = shader});
    RI::setName(device, pipeline, "printf pipeline");
    RI::CommandList cmd = RI::createCommandList(device);
    RI::setName(device, cmd, "printf commands");
    RI::cmdBegin(cmd);
    RI::cmdBeginMarker(cmd, "printf");
    RI::cmdSetHeap(cmd, heap);
    RI::cmdSetPipeline(cmd, pipeline);
    RI::cmdDispatch(cmd, 1, 1, 1);
    RI::cmdEndMarker(cmd);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
    RI::destroyCommandList(device, cmd);
    RI::destroyPipeline(device, pipeline);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroyDevice(device);
}

#endif

namespace {

struct WriteRoot {
    uint32 textureSlot;
    uint32 width;
    uint32 height;
    float value;
};

// One-shot recording on a fresh command list, submitted and waited on.
template <typename Record>
void runOnce(RI::Device &device, Record record) {
    RI::CommandList cmd = RI::createCommandList(device);
    RI::cmdBegin(cmd);
    record(cmd);
    RI::cmdEnd(cmd);
    RI::wait(device.graphicsQueue, RI::submit(device.graphicsQueue, cmd));
    RI::destroyCommandList(device, cmd);
}

} // namespace

// Native sections (P27, P70): native code clears a texture and leaves it in
// TRANSFER_DST (declared on exit), and disturbs the root data; after endNative
// RI's dispatch uses RI's state again. The storage descriptor is written natively (P77 (2)).
EQ_TEST(vk, native_section) {
    RI::Device device = RI::createDevice({.gpu = gputest::pickGpu()});
    gputest::Arena gpu{.memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 16 << 20)};
    gputest::Arena readback{.memory = RI::allocateMemory(device, RI::MemoryKind::eReadback, 1 << 20)};
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 8});
    RI::Texture storage = gputest::placeTexture(device, gpu, {.width = 16, .height = 16, .format = RI::Format::eRGBA32Float,
                                                              .usage = RI::TextureUsage::eStorage | RI::TextureUsage::eCopySrc});
    RI::Texture cleared = gputest::placeTexture(device, gpu, {.width = 16, .height = 16, .format = RI::Format::eRGBA8Unorm,
                                                              .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eCopySrc |
                                                                       RI::TextureUsage::eCopyDst});
    const VkDescriptorImageInfo storageInfo{.imageView = RI::Vk::native(storage.view), .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    RI::Vk::writeNativeDescriptor(device, heap, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &storageInfo, nullptr);
    const std::vector<std::byte> shader = gputest::loadShader("write_storage");
    RI::Pipeline write = RI::createComputePipeline(device, {.shader = shader});
    const RI::CopyLayout storageLayout = RI::copyLayout(device, storage.view);
    const RI::CopyLayout clearedLayout = RI::copyLayout(device, cleared.view);
    RI::Buffer storageBack = gputest::placeBuffer(device, readback, {.size = storageLayout.size, .usage = RI::BufferUsage::eCopyDst});
    RI::Buffer clearedBack = gputest::placeBuffer(device, readback, {.size = clearedLayout.size, .usage = RI::BufferUsage::eCopyDst});

    runOnce(device, [&](RI::CommandList &cmd) {
        RI::cmdSetHeap(cmd, heap);
        RI::cmdSetPipeline(cmd, write);
        RI::Helpers::cmdSetRootData(cmd, WriteRoot{.textureSlot = 2, .width = 16, .height = 16, .value = 9.0f});
        const RI::Vk::NativeCommands n = RI::Vk::beginNative(cmd);
        const WriteRoot wrong{.textureSlot = 2, .width = 16, .height = 16, .value = 1.0f};
        n.functions->vkCmdPushConstants(n.commandBuffer, RI::Vk::native(write).layout, VK_SHADER_STAGE_ALL, 0,
                                        sizeof(wrong), &wrong);
        const VkImageLayout resting = RI::Vk::native(cleared).restingLayout;
        const VkImageMemoryBarrier2 toTransfer{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                               .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                               .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                               .oldLayout = resting,
                                               .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                               .image = RI::Vk::native(cleared).image,
                                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .imageMemoryBarrierCount = 1,
                                          .pImageMemoryBarriers = &toTransfer};
        n.functions->vkCmdPipelineBarrier2(n.commandBuffer, &dependency);
        const VkClearColorValue half{.float32 = {0.5f, 0.5f, 0.5f, 0.5f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        n.functions->vkCmdClearColorImage(n.commandBuffer, RI::Vk::native(cleared).image,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &half, 1, &range);
        const RI::Vk::NativeLayoutChange left[] = {{cleared.view, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}};
        RI::Vk::endNative(cmd, left);
        RI::cmdDispatch(cmd, 2, 2, 1); // RI's root data again: 9
        RI::cmdBarrier(cmd, RI::Stage::eAll, RI::Stage::eCopy);
        RI::cmdCopyTextureToBuffer(cmd, storage.view, storageBack, 0);
        RI::cmdCopyTextureToBuffer(cmd, cleared.view, clearedBack, 0);
    });
    const auto *texels = static_cast<const float *>(storageBack.mapped);
    const auto *bytes = static_cast<const uint8 *>(clearedBack.mapped);
    uint32 wrong = 0;
    for (uint32 y = 0; y < 16; ++y) {
        const auto *row = reinterpret_cast<const float *>(reinterpret_cast<const uint8 *>(texels) + y * storageLayout.rowPitch);
        const uint8 *clearedRow = bytes + y * clearedLayout.rowPitch;
        for (uint32 x = 0; x < 16; ++x) {
            wrong += row[x * 4 + 2] != 9.0f;
            wrong += clearedRow[x * 4] < 126 || clearedRow[x * 4] > 129;
        }
    }
    EQ_CHECK(wrong == 0);

    RI::destroyBuffer(device, storageBack);
    RI::destroyBuffer(device, clearedBack);
    RI::destroyPipeline(device, write);
    RI::destroyTexture(device, cleared);
    RI::destroyTexture(device, storage);
    RI::destroyDescriptorHeap(device, heap);
    RI::freeMemory(device, readback.memory);
    RI::freeMemory(device, gpu.memory);
    RI::destroyDevice(device);
}

// Import (P77): a native image, buffer, memory and pipeline used by RI; RI
// forgets them on destroy and the native objects stay valid.
EQ_TEST(vk, import_objects) {
    RI::Device device = RI::createDevice({.gpu = gputest::pickGpu()});
    const RI::Vk::NativeDevice nd = RI::Vk::native(device);
    const VolkDeviceTable &fn = *nd.functions;
    RI::Memory gpuMemory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 4 << 20);
    RI::Memory uploadMemory = RI::allocateMemory(device, RI::MemoryKind::eUpload, 1 << 20);

    // A native image in RI memory, imported.
    const RI::TextureDesc desc{.width = 16, .height = 16, .format = RI::Format::eRGBA8Unorm,
                               .usage = RI::TextureUsage::eSampled | RI::TextureUsage::eCopySrc | RI::TextureUsage::eCopyDst};
    const VkImageCreateInfo imageInfo{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                      .imageType = VK_IMAGE_TYPE_2D,
                                      .format = VK_FORMAT_R8G8B8A8_UNORM,
                                      .extent = {16, 16, 1},
                                      .mipLevels = 1,
                                      .arrayLayers = 1,
                                      .samples = VK_SAMPLE_COUNT_1_BIT,
                                      .tiling = VK_IMAGE_TILING_OPTIMAL,
                                      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                               VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image = VK_NULL_HANDLE;
    EQ_CHECK(fn.vkCreateImage(nd.device, &imageInfo, nullptr, &image) == VK_SUCCESS);
    EQ_CHECK(fn.vkBindImageMemory(nd.device, image, RI::Vk::native(gpuMemory), 0) == VK_SUCCESS);
    RI::Texture texture = RI::Vk::importTexture(device, image, desc);

    // A native buffer in RI upload memory, imported, as the copy source.
    const VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                        .size = 64 * 1024,
                                        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    VkBuffer nativeBuffer = VK_NULL_HANDLE;
    EQ_CHECK(fn.vkCreateBuffer(nd.device, &bufferInfo, nullptr, &nativeBuffer) == VK_SUCCESS);
    EQ_CHECK(fn.vkBindBufferMemory(nd.device, nativeBuffer, RI::Vk::native(uploadMemory), 0) == VK_SUCCESS);
    RI::Buffer staging = RI::Vk::importBuffer(device, nativeBuffer, {.size = 64 * 1024, .usage = RI::BufferUsage::eCopySrc},
                                              uploadMemory.mapped);
    EQ_CHECK(staging.gpuAddress != 0);

    // Native memory, imported; RI places its readback buffer in it.
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(nd.physicalDevice, &properties);
    uint32 type = UINT32_MAX;
    for (uint32 i = 0; i < properties.memoryTypeCount && type == UINT32_MAX; ++i) {
        const VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 && (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0) {
            type = i;
        }
    }
    const VkMemoryAllocateFlagsInfo addressFlags{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                                 .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    const VkMemoryAllocateInfo allocInfo{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                         .pNext = &addressFlags,
                                         .allocationSize = 1 << 20,
                                         .memoryTypeIndex = type};
    VkDeviceMemory nativeMemory = VK_NULL_HANDLE;
    EQ_CHECK(fn.vkAllocateMemory(nd.device, &allocInfo, nullptr, &nativeMemory) == VK_SUCCESS);
    void *mapped = nullptr;
    EQ_CHECK(fn.vkMapMemory(nd.device, nativeMemory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS);
    RI::Memory readbackMemory = RI::Vk::importMemory(device, nativeMemory, RI::MemoryKind::eReadback, 1 << 20, mapped);
    RI::Buffer back = RI::createBuffer(device, readbackMemory, 0, {.size = 64 * 1024, .usage = RI::BufferUsage::eCopyDst});

    const RI::CopyLayout layout = RI::copyLayout(device, texture.view);
    auto *bytes = static_cast<uint8 *>(staging.mapped);
    for (uint64 i = 0; i < layout.size; ++i) {
        bytes[i] = static_cast<uint8>(i * 7);
    }
    runOnce(device, [&](RI::CommandList &cmd) {
        // The imported image goes to RI's resting layout before RI uses it.
        const RI::Vk::NativeCommands n = RI::Vk::beginNative(cmd);
        const VkImageMemoryBarrier2 toRest{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                           .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                           .newLayout = RI::Vk::native(texture).restingLayout,
                                           .image = image,
                                           .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .imageMemoryBarrierCount = 1,
                                          .pImageMemoryBarriers = &toRest};
        n.functions->vkCmdPipelineBarrier2(n.commandBuffer, &dependency);
        RI::Vk::endNative(cmd);
        RI::cmdCopyBufferToTexture(cmd, staging, 0, texture.view);
        RI::cmdBarrier(cmd, RI::Stage::eCopy, RI::Stage::eCopy);
        RI::cmdCopyTextureToBuffer(cmd, texture.view, back, 0);
    });
    const auto *out = static_cast<const uint8 *>(back.mapped);
    uint32 wrong = 0;
    for (uint32 y = 0; y < 16; ++y) {
        for (uint32 x = 0; x < 64; ++x) {
            const uint64 at = y * layout.rowPitch + x;
            wrong += out[at] != static_cast<uint8>(at * 7);
        }
    }
    EQ_CHECK(wrong == 0);

    // An RI pipeline's native handles, imported a second time: destroying the
    // import only forgets it.
    const std::vector<std::byte> shader = gputest::loadShader("write_storage");
    RI::Pipeline pipeline = RI::createComputePipeline(device, {.shader = shader});
    RI::Pipeline imported = RI::Vk::importPipeline(device, RI::Vk::native(pipeline).pipeline,
                                                   RI::Vk::native(pipeline).layout, false, pipeline.rootDataSize);
    EQ_CHECK(imported.rootDataSize == sizeof(WriteRoot));
    RI::destroyPipeline(device, imported);
    RI::destroyPipeline(device, pipeline);

    RI::destroyBuffer(device, back);
    RI::freeMemory(device, readbackMemory); // forgets only
    RI::destroyBuffer(device, staging);     // forgets only
    RI::destroyTexture(device, texture);    // destroys RI's view, forgets the image
    fn.vkDestroyImage(nd.device, image, nullptr);
    fn.vkDestroyBuffer(nd.device, nativeBuffer, nullptr);
    fn.vkFreeMemory(nd.device, nativeMemory, nullptr);
    RI::freeMemory(device, uploadMemory);
    RI::freeMemory(device, gpuMemory);
    RI::destroyDevice(device);
}

// Work graphs are D3D12 only (P50): a readable unsupported error on Vulkan.
EQ_TEST(vk, work_graph_unsupported_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    RI::Device device = RI::createDevice({.gpu = gputest::pickGpu()});
    EQ_CHECK(!device.limits.workGraphs);
    RI::createWorkGraph(device, {});
}

// Bindful constant buffers after a native section (P70, P112): native code
// pushes another block into slot 3; endNative pushes RI's back.
EQ_TEST(vk, constant_buffers_after_native) {
    RI::Device device = RI::createDevice({.gpu = gputest::pickGpu()});
    gputest::Arena gpu{.memory = RI::allocateMemory(device, RI::MemoryKind::eGPU, 1 << 20)};
    gputest::Arena upload{.memory = RI::allocateMemory(device, RI::MemoryKind::eUpload, 1 << 20)};
    gputest::Arena readback{.memory = RI::allocateMemory(device, RI::MemoryKind::eReadback, 1 << 20)};
    RI::Buffer blocks = gputest::placeBuffer(device, upload, {.size = 1024, .usage = RI::BufferUsage::eConstant});
    const float scale[4] = {2.0f, 0.0f, 0.0f, 0.0f};
    const float kept[8] = {0.0f, 10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 100.0f, 0.0f};
    const float wrong[8] = {0.0f, 20.0f, 0.0f, 0.0f, 0.0f, 0.0f, 200.0f, 0.0f};
    auto *bytes = static_cast<std::byte *>(blocks.mapped);
    std::memcpy(bytes, scale, sizeof(scale));
    std::memcpy(bytes + 256, kept, sizeof(kept));
    std::memcpy(bytes + 512, wrong, sizeof(wrong));
    RI::Buffer output = gputest::placeBuffer(device, gpu, {.size = 256, .usage = RI::BufferUsage::eReadWrite | RI::BufferUsage::eCopySrc});
    RI::Buffer result = gputest::placeBuffer(device, readback, {.size = 256, .usage = RI::BufferUsage::eCopyDst});
    RI::DescriptorHeap heap = RI::createDescriptorHeap(device, {.resourceCount = 4});
    RI::writeDescriptors(device, heap, {{.slot = 0, .descriptor = RI::ReadWriteView{.buffer = output, .stride = 4}}});
    const std::vector<std::byte> shader = gputest::loadShader("constant_buffers");
    RI::Pipeline pipeline = RI::createComputePipeline(device, {.shader = shader});
    struct ConstantRoot {
        uint32 outputSlot;
        uint32 base;
    };
    runOnce(device, [&](RI::CommandList &cmd) {
        RI::cmdSetHeap(cmd, heap);
        RI::cmdSetPipeline(cmd, pipeline);
        RI::cmdSetConstantBuffer(cmd, 0, blocks, 0);
        RI::cmdSetConstantBuffer(cmd, 3, blocks, 256);
        RI::Helpers::cmdSetRootData(cmd, ConstantRoot{.outputSlot = 0, .base = 0});
        const RI::Vk::NativeCommands n = RI::Vk::beginNative(cmd);
        const VkDescriptorBufferInfo info{.buffer = RI::Vk::native(blocks), .offset = 512, .range = 32};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstBinding = 3,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                         .pBufferInfo = &info};
        n.functions->vkCmdPushDescriptorSet(n.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                            RI::Vk::native(pipeline).layout, 2, 1, &write);
        RI::Vk::endNative(cmd, {});
        RI::cmdDispatch(cmd, 1, 1, 1);
        RI::cmdBarrier(cmd, RI::Stage::eCompute, RI::Stage::eCopy);
        RI::cmdCopyBuffer(cmd, output, 0, result, 0, 256);
    });
    const auto *out = static_cast<const float *>(result.mapped);
    uint32 bad = 0;
    for (uint32 i = 0; i < 64; ++i) {
        bad += out[i] != 2.0f * float(i) + 110.0f;
    }
    EQ_CHECK(bad == 0);
    RI::destroyPipeline(device, pipeline);
    RI::destroyDescriptorHeap(device, heap);
    RI::destroyBuffer(device, blocks);
    RI::destroyBuffer(device, output);
    RI::destroyBuffer(device, result);
    RI::freeMemory(device, gpu.memory);
    RI::freeMemory(device, upload.memory);
    RI::freeMemory(device, readback.memory);
    RI::destroyDevice(device);
}

// Aborts by design: set 2 holds EQ_CONSTANT_BUFFER slots 0 to 7 only (P112).
EQ_TEST(vk, constant_buffer_bad_slot_reports) {
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    RI::Device device = RI::createDevice({.gpu = gputest::pickGpu()});
    const std::vector<std::byte> shader = gputest::loadShader("constant_buffer_bad");
    RI::createComputePipeline(device, {.shader = shader});
}
