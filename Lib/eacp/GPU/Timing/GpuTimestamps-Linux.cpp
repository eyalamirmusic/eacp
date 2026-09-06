#include "GpuTimestamps.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanTypes.h"

// Linux/Vulkan backend. Each slot owns one timestamp query pool. Unlike D3D12
// there is no readback buffer and no resolve step: vkGetQueryPoolResults copies
// straight to the CPU once the queries have been written, so what the command
// buffer records is only the reset and the timestamps themselves.
//
// Query layout per slot: two per timed pass from index 0, then the frame's own
// pair at the top. The frame pair is separate for the reason it is on D3D12 -
// there is no equivalent of MTLCommandBuffer's GPUStartTime, so the
// whole-frame number is two more timestamps on the same command buffer, taken
// before anything is recorded and after everything is.
//
// The pool must be reset on a command buffer before anything writes to it (the
// host-side vkResetQueryPool is a 1.2 feature that is not part of the floor
// here), and beginRecording is the one place that is guaranteed to run once per
// slot with a command buffer in hand.

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

    // Deferred for the same reason as the other two backends': this is built by
    // Device, which is not yet itself when that runs - hence the Device
    // arriving with the first slot rather than at construction.
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

    // Nanoseconds per tick, straight off the device limits.
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

    // The whole pool, because a query that is written without having been reset
    // is undefined and one that is reset without being written reads back as
    // unavailable - and resolveSlot reads them all.
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

// Like D3D12 and unlike Metal, there is nothing to report without the queries:
// the frame is measured with the same timestamps a pass is, so a device that
// cannot take them has no frame total either. Saying so is what keeps the
// timer's slots from filling up with frames that can never be answered.
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

    // Each query comes back as a {value, availability} pair. The pool has room
    // for maxTimedPasses, but a frame writes only the passes it had, and a query
    // the frame never wrote is "unavailable" rather than zero: read without the
    // availability bit, one such query fails the whole call with NOT_READY and
    // leaves every value undefined. With it, the call still answers NOT_READY
    // but writes every availability flag and every value that is there, and an
    // unwritten query reads as the zero toMilliseconds already treats as "no
    // number".
    //
    // No WAIT bit: the caller has already established that the submission
    // completed, and asking the driver to block here would turn a poll into a
    // stall on a frame the profiler was only reading.
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
        // A query the GPU never wrote reads as zero, and a disjoint one can
        // read backwards. Both mean "no number" rather than a duration.
        if (end <= start)
            return 0.0;

        return static_cast<double>(end - start) * static_cast<double>(impl->period)
               / 1'000'000.0;
    };

    for (auto pass = 0; pass < passCount; ++pass)
        milliseconds[pass] = toMilliseconds(tick(pass * 2), tick(pass * 2 + 1));

    const auto frameStart = tick(vulkanFrameStartQuery);
    const auto frameEnd = tick(vulkanFrameEndQuery);

    // Where a device that only claimed to support timestamps is found out.
    // These two are absolute tick counts taken outside any pass, and the slot is
    // only read once its submission has completed - so both reading zero cannot
    // mean a quick frame, only that the writes never landed. Retiring support
    // here rather than reporting zeroes is what keeps the rest honest: endSlot
    // then stops marking slots pending, and supportsPassTimings() tells a
    // profiler the truth before it draws an empty graph.
    if (frameStart == 0 && frameEnd == 0)
        impl->supported = false;

    return toMilliseconds(frameStart, frameEnd);
}
} // namespace eacp::GPU
