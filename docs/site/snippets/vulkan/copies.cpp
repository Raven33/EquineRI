// Canonical Vulkan: a buffer-to-image copy of one mip, with the image moved to
// TRANSFER_DST_OPTIMAL and back to SHADER_READ_ONLY_OPTIMAL around it.
#undef VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>

// --8<-- [start:upload]
void upload(VkCommandBuffer commands, VkBuffer staging, VkImage image, uint32_t width, uint32_t height) {
    VkImageMemoryBarrier2 toTransfer{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                     .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                     .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                     .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                     .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     .image = image,
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                .imageMemoryBarrierCount = 1,
                                .pImageMemoryBarriers = &toTransfer};
    vkCmdPipelineBarrier2(commands, &dependency);
    // bufferRowLength 0: rows are tightly packed (Vulkan's offset only needs
    // to be a multiple of the texel size and of 4).
    const VkBufferImageCopy region{.bufferOffset = 0,
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                   .imageExtent = {width, height, 1}};
    vkCmdCopyBufferToImage(commands, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier2 back = toTransfer;
    back.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    back.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    back.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    back.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dependency.pImageMemoryBarriers = &back;
    vkCmdPipelineBarrier2(commands, &dependency);
}
// --8<-- [end:upload]

// --8<-- [start:small-write]
void resetCounter(VkCommandBuffer commands, VkBuffer counters, uint64_t start) {
    // A transfer command: the buffer needs TRANSFER_DST; at most 65536 bytes,
    // offset and size multiples of 4; not inside a render pass.
    vkCmdUpdateBuffer(commands, counters, 0, sizeof(start), &start);
    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                   .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .memoryBarrierCount = 1,
                                      .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commands, &dependency);
}
// --8<-- [end:small-write]
