#include "GpuTimestamps.h"

#include "../Device/Device.h"
#include "../WebGPU/WebGPUTypes.h"

#include <algorithm>
#include <memory>

namespace eacp::GPU
{
namespace
{
// Only the passes write timestamps - WebGPU has no command-level one - so a
// slot holds a pair per timed pass and nothing for the frame as a whole.
constexpr int webQueriesPerSlot = GpuTimestamps::maxTimedPasses * 2;
constexpr std::uint64_t webSlotBytes = webQueriesPerSlot * sizeof(std::uint64_t);

// What the map callback reaches, which may outlive nothing it touches.
struct WebTimestampMapping
{
    bool mapped = false;
    bool failed = false;
};

void onTimestampsMapped(WGPUMapAsyncStatus status,
                        WGPUStringView,
                        void* userdata,
                        void*)
{
    auto* weak = static_cast<std::weak_ptr<WebTimestampMapping>*>(userdata);

    if (auto mapping = weak->lock())
    {
        mapping->mapped = status == WGPUMapAsyncStatus_Success;
        mapping->failed = !mapping->mapped;
    }

    delete weak;
}

WGPUBuffer makeTimestampBuffer(WGPUBufferUsage usage)
{
    auto descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    descriptor.usage = usage;
    descriptor.size = webSlotBytes;

    return wgpuDeviceCreateBuffer(getWebGPUShared().getDevice(), &descriptor);
}
} // namespace

struct GpuTimestamps::Native
{
    struct Slot
    {
        WGPUQuerySet querySet = nullptr;
        WGPUBuffer resolve = nullptr;
        WGPUBuffer readback = nullptr;
        std::shared_ptr<WebTimestampMapping> mapping =
            std::make_shared<WebTimestampMapping>();
        std::uint64_t bytes = 0;
        bool submitted = false;
    };

    // Deferred: this is built by Device, which is not yet itself when that runs.
    void ensureCreated(Device& device)
    {
        if (tried)
            return;

        tried = true;

        auto& shared = getWebGPUShared();

        if (!device.isValid() || !shared.hasFeature(WGPUFeatureName_TimestampQuery))
            return;

        for (auto& slot: slots)
        {
            auto descriptor = WGPU_QUERY_SET_DESCRIPTOR_INIT;
            descriptor.type = WGPUQueryType_Timestamp;
            descriptor.count = webQueriesPerSlot;

            slot.querySet =
                wgpuDeviceCreateQuerySet(shared.getDevice(), &descriptor);
            slot.resolve = makeTimestampBuffer(WGPUBufferUsage_QueryResolve
                                               | WGPUBufferUsage_CopySrc);
            slot.readback = makeTimestampBuffer(WGPUBufferUsage_MapRead
                                                | WGPUBufferUsage_CopyDst);

            if (slot.querySet == nullptr || slot.resolve == nullptr
                || slot.readback == nullptr)
                return;
        }

        supported = true;
    }

    ~Native()
    {
        for (auto& slot: slots)
        {
            if (slot.readback != nullptr)
                wgpuBufferRelease(slot.readback);

            if (slot.resolve != nullptr)
                wgpuBufferRelease(slot.resolve);

            if (slot.querySet != nullptr)
                wgpuQuerySetRelease(slot.querySet);
        }
    }

    Array<Slot, slotCount> slots;
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

    auto& entry = impl->slots[slot];

    if (entry.mapping->mapped)
        wgpuBufferUnmap(entry.readback);

    entry.mapping->mapped = false;
    entry.mapping->failed = false;
    entry.submitted = false;
    entry.bytes = 0;
}

void GpuTimestamps::beginRecording(int, void*) {}

void* GpuTimestamps::nativeSamples(int slot) const
{
    if (!impl->supported)
        return nullptr;

    return impl->slots[slot].querySet;
}

// Resolved and copied out at the end of the recording that timed the passes.
bool GpuTimestamps::endSlot(int slot, int passCount, void* nativeCommandBuffer)
{
    auto encoder = static_cast<WGPUCommandEncoder>(nativeCommandBuffer);

    if (!impl->supported || encoder == nullptr || passCount <= 0)
        return false;

    auto& entry = impl->slots[slot];
    const auto queries = static_cast<std::uint32_t>(passCount * 2);

    entry.bytes = queries * sizeof(std::uint64_t);

    wgpuCommandEncoderResolveQuerySet(
        encoder, entry.querySet, 0, queries, entry.resolve, 0);
    wgpuCommandEncoderCopyBufferToBuffer(
        encoder, entry.resolve, 0, entry.readback, 0, entry.bytes);

    return true;
}

// The map is requested once the copy is on the queue, and lands a frame or
// more later, which is what FrameTimer expects of every backend.
void GpuTimestamps::noteSubmitted(int slot, std::uint64_t)
{
    if (!impl->supported)
        return;

    auto& entry = impl->slots[slot];

    if (entry.bytes == 0 || entry.submitted)
        return;

    entry.submitted = true;

    auto info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_AllowSpontaneous;
    info.callback = onTimestampsMapped;
    info.userdata1 = new std::weak_ptr<WebTimestampMapping>(entry.mapping);

    wgpuBufferMapAsync(entry.readback,
                       WGPUMapMode_Read,
                       0,
                       static_cast<std::size_t>(entry.bytes),
                       info);
}

bool GpuTimestamps::isSlotComplete(int slot, const Device&) const
{
    const auto& entry = impl->slots[slot];

    return entry.submitted && (entry.mapping->mapped || entry.mapping->failed);
}

// The frame's time is from the first timed pass beginning to the last ending.
double GpuTimestamps::resolveSlot(int slot, int passCount, double* milliseconds)
{
    auto& entry = impl->slots[slot];

    if (!impl->supported || !entry.mapping->mapped)
        return 0.0;

    const auto* ticks =
        static_cast<const std::uint64_t*>(wgpuBufferGetConstMappedRange(
            entry.readback, 0, static_cast<std::size_t>(entry.bytes)));

    if (ticks == nullptr)
        return 0.0;

    const auto toMilliseconds = [](std::uint64_t start, std::uint64_t end)
    {
        if (start == 0 || end <= start)
            return 0.0;

        return static_cast<double>(end - start) / 1'000'000.0;
    };

    const auto count = std::min<std::uint64_t>(static_cast<std::uint64_t>(passCount),
                                               entry.bytes / 16);

    auto first = std::uint64_t {0};
    auto last = std::uint64_t {0};

    for (auto pass = std::uint64_t {0}; pass < count; ++pass)
    {
        const auto begin = ticks[pass * 2];
        const auto end = ticks[pass * 2 + 1];

        milliseconds[pass] = toMilliseconds(begin, end);

        if (begin != 0 && (first == 0 || begin < first))
            first = begin;

        last = std::max(last, end);
    }

    wgpuBufferUnmap(entry.readback);
    entry.mapping->mapped = false;

    return toMilliseconds(first, last);
}
} // namespace eacp::GPU
