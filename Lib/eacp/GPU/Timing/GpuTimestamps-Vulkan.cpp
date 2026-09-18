#include "GpuTimestamps.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanBackend-Linux.h"
#include "../Vulkan/VulkanTypes.h"

namespace eacp::GPU
{
namespace
{
constexpr int vulkanFrameStartQuery = GpuTimestamps::maxTimedPasses * 2;
constexpr int vulkanFrameEndQuery = vulkanFrameStartQuery + 1;
constexpr int vulkanQueriesPerSlot = vulkanFrameEndQuery + 1;

struct VulkanGpuTimestamps final : GpuTimestampsBackend
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

    ~VulkanGpuTimestamps() override
    {
        if (device == VK_NULL_HANDLE)
            return;

        for (auto& slot: slots)
            if (slot.pool != VK_NULL_HANDLE)
                vkDestroyQueryPool(device, slot.pool, nullptr);
    }

    Array<Slot, GpuTimestamps::slotCount> slots;

    VkDevice device = VK_NULL_HANDLE;

    // Nanoseconds per tick.
    float period = 0.f;

    bool supported = false;
    bool tried = false;


    bool isSupported() const override
    {
        return supported;
    }

    void beginSlot(int slot, Device& device) override
    {
        ensureCreated(device);

        if (!supported)
            return;

        slots[slot].submitted = false;
    }

    void beginRecording(int slot, void* nativeCommandBuffer) override
    {
        if (!supported)
            return;

        auto commandBuffer = static_cast<VkCommandBuffer>(nativeCommandBuffer);

        if (commandBuffer == VK_NULL_HANDLE)
            return;

        auto& entry = slots[slot];

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

    void* nativeSamples(int slot) const override
    {
        if (!supported)
            return nullptr;

        return slots[slot].pool;
    }

    bool endSlot(int slot, int, void* nativeCommandBuffer) override
    {
        if (!supported)
            return false;

        auto commandBuffer = static_cast<VkCommandBuffer>(nativeCommandBuffer);

        if (commandBuffer == VK_NULL_HANDLE)
            return false;

        vkCmdWriteTimestamp2(commandBuffer,
                             VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                             slots[slot].pool,
                             static_cast<std::uint32_t>(vulkanFrameEndQuery));

        return true;
    }

    void noteSubmitted(int slot, std::uint64_t completionValue) override
    {
        if (!supported)
            return;

        slots[slot].completionValue = completionValue;
        slots[slot].submitted = true;
    }

    bool isSlotComplete(int slot, const Device& device) const override
    {
        const auto& entry = slots[slot];

        return entry.submitted
               && getVulkanContext(device).hasCompleted(entry.completionValue);
    }

    double resolveSlot(int slot, int passCount, double* milliseconds) override
    {
        const auto& entry = slots[slot];

        if (!supported || entry.pool == VK_NULL_HANDLE)
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
            device,
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

            return static_cast<double>(end - start) * static_cast<double>(period)
                   / 1'000'000.0;
        };

        for (auto pass = 0; pass < passCount; ++pass)
            milliseconds[pass] = toMilliseconds(tick(pass * 2), tick(pass * 2 + 1));

        const auto frameStart = tick(vulkanFrameStartQuery);
        const auto frameEnd = tick(vulkanFrameEndQuery);

        // Absolute ticks, read once the submission completed, so both being zero
        // cannot mean a quick frame - only that the device never wrote them.
        if (frameStart == 0 && frameEnd == 0)
            supported = false;

        return toMilliseconds(frameStart, frameEnd);
    }
};
} // namespace

std::unique_ptr<GpuTimestampsBackend> makeVulkanGpuTimestamps()
{
    return std::make_unique<VulkanGpuTimestamps>();
}
} // namespace eacp::GPU
