#include "OcclusionQueries.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanTypes.h"

#include <algorithm>
#include <cstring>

// The Vulkan backend. An OCCLUSION query pool of the set's size and a host
// buffer the results are copied into: each pass copies out the queries it
// used once rendering has ended, and resets them there, which is the only
// place a query may be reset and what lets the next pass or frame begin them
// again. The pool's first reset, for queries never yet used, is recorded by
// the first pass that names the set, before it begins rendering.

namespace eacp::GPU
{
struct OcclusionQueries::Native
{
    // See the D3D12 backend: a recording recycled under a new id has finished
    // or will never run; one with no completion value is still being recorded.
    enum class Writer
    {
        Done,
        Running,
        Pending
    };

    Writer writerState() const
    {
        if (recording == nullptr || recording->recordingId != recordingId)
            return Writer::Done;

        if (recording->completionValue == 0)
            return Writer::Pending;

        return getVulkanContext(*device).hasCompleted(recording->completionValue)
                   ? Writer::Done
                   : Writer::Running;
    }

    VkQueryPool pool = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    std::byte* mapped = nullptr;

    // Whether the whole pool's first reset has been recorded.
    bool poolReset = false;

    CommandContext* recording = nullptr;
    std::uint64_t recordingId = 0;

    Device* device = nullptr;
    int count = 0;
};

OcclusionQueries::OcclusionQueries(Device& device, int count)
{
    impl->device = &device;

    if (count < 1 || !device.supportsOcclusionQueries())
        return;

    auto& context = getVulkanContext(device);

    if (!context.isValid())
        return;

    VkQueryPoolCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_OCCLUSION;
    info.queryCount = static_cast<std::uint32_t>(count);

    if (vkCreateQueryPool(context.getDevice(), &info, nullptr, &impl->pool)
        != VK_SUCCESS)
    {
        impl->pool = VK_NULL_HANDLE;
        return;
    }

    const auto bytes = static_cast<std::size_t>(count) * sizeof(std::uint64_t);

    if (!context.makeHostBuffer(bytes,
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                true,
                                impl->buffer,
                                impl->allocation,
                                impl->mapped))
    {
        vkDestroyQueryPool(context.getDevice(), impl->pool, nullptr);
        impl->pool = VK_NULL_HANDLE;
        impl->buffer = VK_NULL_HANDLE;
        impl->mapped = nullptr;
        return;
    }

    std::memset(impl->mapped, 0, bytes);
    impl->count = count;
}

OcclusionQueries::~OcclusionQueries()
{
    if (impl->mapped == nullptr)
        return;

    auto& context = getVulkanContext(*impl->device);
    context.deferReleaseBuffer(impl->buffer, impl->allocation);
    context.deferRelease([device = context.getDevice(), pool = impl->pool]
                         { vkDestroyQueryPool(device, pool, nullptr); });
}

bool OcclusionQueries::isValid() const
{
    return impl->mapped != nullptr;
}

int OcclusionQueries::size() const
{
    return impl->count;
}

bool OcclusionQueries::isComplete() const
{
    return impl->writerState() == Native::Writer::Done;
}

void OcclusionQueries::read(std::span<std::uint64_t> results, int first) const
{
    if (impl->mapped == nullptr || first < 0 || first >= impl->count
        || results.empty())
        return;

    impl->device->assertOwningThread();

    if (impl->writerState() == Native::Writer::Running)
        getVulkanContext(*impl->device).waitFor(impl->recording->completionValue);

    const auto available = static_cast<std::size_t>(impl->count - first);
    const auto copied = std::min(results.size(), available);
    const auto* source =
        reinterpret_cast<const std::uint64_t*>(impl->mapped) + first;

    std::memcpy(results.data(), source, copied * sizeof(std::uint64_t));
}

void* OcclusionQueries::nativeResults() const
{
    return impl->buffer;
}

void* OcclusionQueries::nativeQueries() const
{
    return impl->pool;
}

void OcclusionQueries::notePass(void* nativeRecording)
{
    auto* commands = static_cast<CommandContext*>(nativeRecording);

    if (commands == nullptr || impl->pool == VK_NULL_HANDLE)
        return;

    impl->recording = commands;
    impl->recordingId = commands->recordingId;

    if (impl->poolReset)
        return;

    vkCmdResetQueryPool(
        commands->buffer, impl->pool, 0, static_cast<std::uint32_t>(impl->count));
    impl->poolReset = true;
}
} // namespace eacp::GPU
