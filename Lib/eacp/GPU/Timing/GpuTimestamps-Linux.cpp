#include "GpuTimestamps.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanTypes.h"

namespace eacp::GPU
{
namespace
{
constexpr int vulkanFrameStartQuery = GpuTimestamps::maxTimedPasses * 2;
constexpr int vulkanFrameEndQuery = vulkanFrameStartQuery + 1;
constexpr int vulkanQueriesPerSlot = vulkanFrameEndQuery + 1;
} // namespace

struct GpuTimestamps::Native
{
    struct Slot
    {
        VkQueryPool pool = VK_NULL_HANDLE;
        std::uint64_t completionValue = 0;
        bool submitted = false;
    };

    // Deferred: this is built by Device, which is not yet itself when that runs.
    void ensureCreated(GPU::Device& owner)
    {
        if (tried)
            return;

        tried = true;

        auto& context = getVulkanContext(owner);

        if (!context.isValid() || !getVulkanShared().supportsTimestamps())
            return;

        device = context.getDevice();
        period = getVulkanShared().getTimestampPeriod();

        VkQueryPoolCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = static_cast<std::uint32_t>(vulkanQueriesPerSlot);

        for (auto& slot: slots)
            if (vkCreateQueryPool(device, &info, nullptr, &slot.pool) != VK_SUCCESS)
                return;

        supported = period > 0.f;
    }

    ~Native()
    {
        if (device == VK_NULL_HANDLE)
            return;

        for (auto& slot: slots)
            if (slot.pool != VK_NULL_HANDLE)
                vkDestroyQueryPool(device, slot.pool, nullptr);
    }

    Array<Slot, slotCount> slots;

    VkDevice device = VK_NULL_HANDLE;

    // Nanoseconds per tick.
    float period = 0.f;

    bool supported = false;
    bool tried = false;
};

GpuTimestamps::GpuTimestamps() = default;
GpuTimestamps::~GpuTimestamps() = default;

bool GpuTimestamps::isSupported() const
{
    return impl->supported;
}

void GpuTimestamps::beginSlot(int slot, Device& device)
{
    impl->ensureCreated(device);

    if (!impl->supported)
        return;

    impl->slots[slot].submitted = false;
}

void GpuTimestamps::beginRecording(int slot, void* nativeCommandBuffer)
{
    if (!impl->supported)
        return;

    auto commandBuffer = static_cast<VkCommandBuffer>(nativeCommandBuffer);

    if (commandBuffer == VK_NULL_HANDLE)
        return;

    auto& entry = impl->slots[slot];

    // Recorded rather than host-side, vkResetQueryPool being outside the floor.
    // The whole pool, because resolveSlot reads it all.
    vkCmdResetQueryPool(commandBuffer,
                        entry.pool,
                        0,
                        static_cast<std::uint32_t>(vulkanQueriesPerSlot));

    vkCmdWriteTimestamp2(commandBuffer,
                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                         entry.pool,
                         static_cast<std::uint32_t>(vulkanFrameStartQuery));
}

void* GpuTimestamps::nativeSamples(int slot) const
{
    if (!impl->supported)
        return nullptr;

    return impl->slots[slot].pool;
}

bool GpuTimestamps::endSlot(int slot, int, void* nativeCommandBuffer)
{
    if (!impl->supported)
        return false;

    auto commandBuffer = static_cast<VkCommandBuffer>(nativeCommandBuffer);

    if (commandBuffer == VK_NULL_HANDLE)
        return false;

    vkCmdWriteTimestamp2(commandBuffer,
                         VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                         impl->slots[slot].pool,
                         static_cast<std::uint32_t>(vulkanFrameEndQuery));

    return true;
}

void GpuTimestamps::noteSubmitted(int slot, std::uint64_t completionValue)
{
    if (!impl->supported)
        return;

    impl->slots[slot].completionValue = completionValue;
    impl->slots[slot].submitted = true;
}

bool GpuTimestamps::isSlotComplete(int slot, const Device& device) const
{
    const auto& entry = impl->slots[slot];

    return entry.submitted
           && getVulkanContext(device).hasCompleted(entry.completionValue);
}

double GpuTimestamps::resolveSlot(int slot, int passCount, double* milliseconds)
{
    const auto& entry = impl->slots[slot];

    if (!impl->supported || entry.pool == VK_NULL_HANDLE)
        return 0.0;

    // Without the availability bit, one query the frame never wrote fails the
    // whole call. No WAIT bit: the submission is known to have completed.
    struct QueryResult
    {
        std::uint64_t value = 0;
        std::uint64_t available = 0;
    };

    QueryResult results[vulkanQueriesPerSlot] = {};

    const auto status = vkGetQueryPoolResults(
        impl->device,
        entry.pool,
        0,
        static_cast<std::uint32_t>(vulkanQueriesPerSlot),
        sizeof(results),
        results,
        sizeof(QueryResult),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

    if (status != VK_SUCCESS && status != VK_NOT_READY)
        return 0.0;

    const auto tick = [&results](int index)
    {
        return results[index].available != 0 ? results[index].value
                                             : std::uint64_t {0};
    };

    const auto toMilliseconds = [this](std::uint64_t start, std::uint64_t end)
    {
        // An unwritten query reads as zero, a disjoint one backwards.
        if (end <= start)
            return 0.0;

        return static_cast<double>(end - start) * static_cast<double>(impl->period)
               / 1'000'000.0;
    };

    for (auto pass = 0; pass < passCount; ++pass)
        milliseconds[pass] = toMilliseconds(tick(pass * 2), tick(pass * 2 + 1));

    const auto frameStart = tick(vulkanFrameStartQuery);
    const auto frameEnd = tick(vulkanFrameEndQuery);

    // Absolute ticks, read once the submission completed, so both being zero
    // cannot mean a quick frame - only that the device never wrote them.
    if (frameStart == 0 && frameEnd == 0)
        impl->supported = false;

    return toMilliseconds(frameStart, frameEnd);
}
} // namespace eacp::GPU
