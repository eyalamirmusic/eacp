#include "VulkanLegacySync.h"

#include <eacp/Core/Utils/Containers.h>

#include <cassert>
#include <cstdint>

namespace eacp::GPU
{
namespace
{
struct LegacyFlagMapping
{
    VkFlags64 synchronization2;
    VkFlags legacy;
};

// Every synchronization2 bit at or above bit 32 a graphics and compute device
// can be handed. Pre-rasterization is the vertex stage alone: eacp's pipelines
// have no tessellation or geometry stage, and the legacy bits for them need
// features eacp does not enable.
constexpr LegacyFlagMapping legacyStageMappings[] = {
    {VK_PIPELINE_STAGE_2_COPY_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT},
    {VK_PIPELINE_STAGE_2_RESOLVE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT},
    {VK_PIPELINE_STAGE_2_BLIT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT},
    {VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT},
    {VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT},
    {VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
     VK_PIPELINE_STAGE_VERTEX_INPUT_BIT},
    {VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT,
     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT}};

constexpr LegacyFlagMapping legacyAccessMappings[] = {
    {VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_ACCESS_SHADER_READ_BIT},
    {VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_ACCESS_SHADER_READ_BIT},
    {VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT}};

constexpr auto lowerHalf = VkFlags64 {0xffffffffu};

template <std::size_t Count>
VkFlags translateFlags(VkFlags64 flags, const LegacyFlagMapping (&mappings)[Count])
{
    auto legacy = static_cast<VkFlags>(flags & lowerHalf);
    auto upper = flags & ~lowerHalf;

    for (const auto& mapping: mappings)
    {
        if ((upper & mapping.synchronization2) == 0)
            continue;

        legacy |= mapping.legacy;
        upper &= ~mapping.synchronization2;
    }

    assert(upper == 0 && "eacp: a synchronization2 bit with no legacy mapping");

    return legacy;
}

VKAPI_ATTR void VKAPI_CALL legacyCmdPipelineBarrier2(VkCommandBuffer commandBuffer,
                                                     const VkDependencyInfo* info)
{
    auto sourceStages = VkPipelineStageFlags2 {VK_PIPELINE_STAGE_2_NONE};
    auto destinationStages = VkPipelineStageFlags2 {VK_PIPELINE_STAGE_2_NONE};

    auto memory = Vector<VkMemoryBarrier> {};
    auto buffers = Vector<VkBufferMemoryBarrier> {};
    auto images = Vector<VkImageMemoryBarrier> {};

    for (auto i = 0u; i < info->memoryBarrierCount; ++i)
    {
        const auto& from = info->pMemoryBarriers[i];
        sourceStages |= from.srcStageMask;
        destinationStages |= from.dstStageMask;

        VkMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = legacyAccess(from.srcAccessMask);
        barrier.dstAccessMask = legacyAccess(from.dstAccessMask);
        memory.add(barrier);
    }

    for (auto i = 0u; i < info->bufferMemoryBarrierCount; ++i)
    {
        const auto& from = info->pBufferMemoryBarriers[i];
        sourceStages |= from.srcStageMask;
        destinationStages |= from.dstStageMask;

        VkBufferMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = legacyAccess(from.srcAccessMask);
        barrier.dstAccessMask = legacyAccess(from.dstAccessMask);
        barrier.srcQueueFamilyIndex = from.srcQueueFamilyIndex;
        barrier.dstQueueFamilyIndex = from.dstQueueFamilyIndex;
        barrier.buffer = from.buffer;
        barrier.offset = from.offset;
        barrier.size = from.size;
        buffers.add(barrier);
    }

    for (auto i = 0u; i < info->imageMemoryBarrierCount; ++i)
    {
        const auto& from = info->pImageMemoryBarriers[i];
        sourceStages |= from.srcStageMask;
        destinationStages |= from.dstStageMask;

        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = legacyAccess(from.srcAccessMask);
        barrier.dstAccessMask = legacyAccess(from.dstAccessMask);
        barrier.oldLayout = from.oldLayout;
        barrier.newLayout = from.newLayout;
        barrier.srcQueueFamilyIndex = from.srcQueueFamilyIndex;
        barrier.dstQueueFamilyIndex = from.dstQueueFamilyIndex;
        barrier.image = from.image;
        barrier.subresourceRange = from.subresourceRange;
        images.add(barrier);
    }

    // One command: its two scopes are the unions of every barrier's, which
    // orders at least what the separate synchronization2 scopes did.
    vkCmdPipelineBarrier(commandBuffer,
                         legacySourceStages(sourceStages),
                         legacyDestinationStages(destinationStages),
                         info->dependencyFlags,
                         static_cast<std::uint32_t>(memory.size()),
                         memory.data(),
                         static_cast<std::uint32_t>(buffers.size()),
                         buffers.data(),
                         static_cast<std::uint32_t>(images.size()),
                         images.data());
}

VKAPI_ATTR void VKAPI_CALL legacyCmdWriteTimestamp2(VkCommandBuffer commandBuffer,
                                                    VkPipelineStageFlags2 stage,
                                                    VkQueryPool queryPool,
                                                    std::uint32_t query)
{
    vkCmdWriteTimestamp(
        commandBuffer,
        static_cast<VkPipelineStageFlagBits>(legacySourceStages(stage)),
        queryPool,
        query);
}

// The arrays one VkSubmitInfo points into.
struct LegacySubmission
{
    Vector<VkSemaphore> waits;
    Vector<std::uint64_t> waitValues;
    Vector<VkPipelineStageFlags> waitStages;
    Vector<VkCommandBuffer> commandBuffers;
    Vector<VkSemaphore> signals;
    Vector<std::uint64_t> signalValues;
    VkTimelineSemaphoreSubmitInfo timeline = {};
};

// A binary semaphore's value is ignored; synchronization2 callers leave it at
// zero, which is what the timeline struct is then handed. A signal's stage is
// dropped: a legacy signal always waits for the whole batch.
VKAPI_ATTR VkResult VKAPI_CALL legacyQueueSubmit2(VkQueue queue,
                                                  std::uint32_t submitCount,
                                                  const VkSubmitInfo2* submits,
                                                  VkFence fence)
{
    auto submissions = Vector<LegacySubmission> {};
    submissions.resize(static_cast<int>(submitCount));

    auto infos = Vector<VkSubmitInfo> {};

    for (auto i = 0u; i < submitCount; ++i)
    {
        const auto& from = submits[i];
        auto& submission = submissions[static_cast<int>(i)];

        for (auto w = 0u; w < from.waitSemaphoreInfoCount; ++w)
        {
            const auto& wait = from.pWaitSemaphoreInfos[w];
            submission.waits.add(wait.semaphore);
            submission.waitValues.add(wait.value);
            submission.waitStages.add(legacyDestinationStages(wait.stageMask));
        }

        for (auto c = 0u; c < from.commandBufferInfoCount; ++c)
            submission.commandBuffers.add(from.pCommandBufferInfos[c].commandBuffer);

        for (auto s = 0u; s < from.signalSemaphoreInfoCount; ++s)
        {
            const auto& signal = from.pSignalSemaphoreInfos[s];
            submission.signals.add(signal.semaphore);
            submission.signalValues.add(signal.value);
        }

        auto& timeline = submission.timeline;
        timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline.waitSemaphoreValueCount =
            static_cast<std::uint32_t>(submission.waitValues.size());
        timeline.pWaitSemaphoreValues = submission.waitValues.data();
        timeline.signalSemaphoreValueCount =
            static_cast<std::uint32_t>(submission.signalValues.size());
        timeline.pSignalSemaphoreValues = submission.signalValues.data();

        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.pNext = &timeline;
        info.waitSemaphoreCount =
            static_cast<std::uint32_t>(submission.waits.size());
        info.pWaitSemaphores = submission.waits.data();
        info.pWaitDstStageMask = submission.waitStages.data();
        info.commandBufferCount =
            static_cast<std::uint32_t>(submission.commandBuffers.size());
        info.pCommandBuffers = submission.commandBuffers.data();
        info.signalSemaphoreCount =
            static_cast<std::uint32_t>(submission.signals.size());
        info.pSignalSemaphores = submission.signals.data();
        infos.add(info);
    }

    return vkQueueSubmit(queue, submitCount, infos.data(), fence);
}
} // namespace

VkPipelineStageFlags legacyStages(VkPipelineStageFlags2 stages)
{
    return translateFlags(stages, legacyStageMappings);
}

VkPipelineStageFlags legacySourceStages(VkPipelineStageFlags2 stages)
{
    const auto legacy = legacyStages(stages);
    return legacy == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : legacy;
}

VkPipelineStageFlags legacyDestinationStages(VkPipelineStageFlags2 stages)
{
    const auto legacy = legacyStages(stages);
    return legacy == 0 ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : legacy;
}

VkAccessFlags legacyAccess(VkAccessFlags2 access)
{
    return translateFlags(access, legacyAccessMappings);
}

void installLegacySynchronization()
{
    vkCmdPipelineBarrier2 = legacyCmdPipelineBarrier2;
    vkCmdWriteTimestamp2 = legacyCmdWriteTimestamp2;
    vkQueueSubmit2 = legacyQueueSubmit2;
}
} // namespace eacp::GPU
