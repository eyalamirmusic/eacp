#import <Metal/Metal.h>

#include "OcclusionQueries.h"

#include "../Device/Device.h"

#include <eacp/Core/ObjC/ObjC.h>

#include <algorithm>
#include <cstring>

// Metal counts into a visibility result buffer the pass descriptor names, eight
// bytes a query. It is shared storage, so the results are a memcpy away once
// the command buffer has completed, and the GPU writes only the offsets a pass
// set a mode at - which is what lets one set be filled over several passes.

namespace eacp::GPU
{
struct OcclusionQueries::Native
{
    ObjC::Ptr<NSObject<MTLBuffer>> results;

    // The last command buffer a pass naming the set was recorded on. Retained,
    // being autoreleased, so its status can still be asked a frame later.
    ObjC::Ptr<NSObject<MTLCommandBuffer>> lastWriter;

    Device* device = nullptr;
    int count = 0;
};

OcclusionQueries::OcclusionQueries(Device& device, int count)
{
    impl->device = &device;

    if (count < 1 || !device.supportsOcclusionQueries())
        return;

    auto metal = (__bridge id<MTLDevice>) device.nativeDevice();
    const auto bytes = (NSUInteger) count * sizeof(std::uint64_t);
    auto buffer = [metal newBufferWithLength:bytes
                                     options:MTLResourceStorageModeShared];

    if (buffer == nil)
        return;

    std::memset(buffer.contents, 0, bytes);
    impl->results.set((NSObject<MTLBuffer>*) buffer);
    impl->count = count;
}

OcclusionQueries::~OcclusionQueries() = default;

bool OcclusionQueries::isValid() const
{
    return impl->results.get() != nil;
}

int OcclusionQueries::size() const
{
    return impl->count;
}

bool OcclusionQueries::isComplete() const
{
    auto buffer = (id<MTLCommandBuffer>) impl->lastWriter.get();

    if (buffer == nil)
        return true;

    const auto status = buffer.status;

    // Error counts as finished, as it does for the frame timer: nothing more
    // will be written, and a set that never completes is worse than one that
    // reads what it held.
    return status == MTLCommandBufferStatusCompleted
           || status == MTLCommandBufferStatusError;
}

void OcclusionQueries::read(std::span<std::uint64_t> results, int first) const
{
    auto buffer = (id<MTLBuffer>) impl->results.get();

    if (buffer == nil || first < 0 || first >= impl->count || results.empty())
        return;

    impl->device->assertOwningThread();

    // Committed or scheduled: on its way, so worth the wait. Not yet enqueued
    // is a frame still being recorded, and waiting on that would never return.
    if (auto writer = (id<MTLCommandBuffer>) impl->lastWriter.get())
        if (writer.status == MTLCommandBufferStatusCommitted
            || writer.status == MTLCommandBufferStatusScheduled)
            [writer waitUntilCompleted];

    const auto available = (std::size_t) (impl->count - first);
    const auto copied = std::min(results.size(), available);
    const auto* source = (const std::uint64_t*) buffer.contents + first;

    std::memcpy(results.data(), source, copied * sizeof(std::uint64_t));
}

void* OcclusionQueries::nativeResults() const
{
    return (__bridge void*) impl->results.get();
}

void* OcclusionQueries::nativeQueries() const
{
    // Metal has no query object beside the buffer.
    return nullptr;
}

void OcclusionQueries::notePass(void* nativeRecording)
{
    if (nativeRecording != nullptr)
        impl->lastWriter.reset((__bridge NSObject<MTLCommandBuffer>*) nativeRecording);
}
} // namespace eacp::GPU
