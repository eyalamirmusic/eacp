#pragma once

#include <eacp/Core/Utils/Pimpl.h>

#include <cstdint>
#include <span>

namespace eacp::GPU
{
class Device;

// What an occlusion query counts. Both count the samples of the draws inside
// the query that passed the depth and stencil tests; they differ in what the
// number is worth.
enum class OcclusionMode
{
    // How many samples passed: the pixels a draw covered and kept, times the
    // target's sample count. Exact where Device::supportsOcclusionSampleCounts()
    // says so; elsewhere (a Vulkan device without occlusionQueryPrecise) only
    // zero or not zero is promised, as for AnySamples.
    SampleCount,

    // Whether any sample passed: zero when none did, and something other than
    // zero - not a count - otherwise. The cheaper question, which is all a
    // renderer deciding whether to draw an object next frame asks.
    AnySamples
};

// A set of occlusion queries and the 64-bit results they count into: what a
// RenderPass draws into when RenderPassDescriptor::occlusionQueries names the
// set, one query open at a time over the draws it measures.
//
//     auto queries = OcclusionQueries {device, 64};
//
//     auto pass = frame.beginPass({.occlusionQueries = &queries});
//     pass.beginOcclusionQuery(3);
//     pass.draw(boundingBox);
//     pass.endOcclusionQuery();
//
//     // a frame or more later
//     if (queries.isComplete() && queries.result(3) == 0)
//         skipTheObject();
//
// **The results are written when the pass ends, and are readable once the GPU
// has finished the frame that recorded it** - isComplete() asks that without
// blocking, and read() and result() wait for it. A query not begun in that
// frame keeps whatever it held before, so a set can be written a few indices
// at a time over several passes and frames. One index is begun at most once
// per pass.
//
// The set is one buffer of results, not a ring of them. A renderer that reads
// frame N's answers while frame N+1 is writing the same indices keeps one set
// per frame in flight, as the frame timer keeps its slots - otherwise
// isComplete() is asking about the newest frame, which is still running.
//
// Metal counts with the pass's visibility result buffer (setVisibilityResultMode
// per query, Counting or Boolean at the query's offset); D3D12 with an
// OCCLUSION query heap, BeginQuery and EndQuery of OCCLUSION or
// BINARY_OCCLUSION, and a ResolveQueryData into a readback buffer at the end of
// the pass; Vulkan with an OCCLUSION query pool, vkCmdBeginQuery with or
// without the PRECISE bit, and vkCmdCopyQueryPoolResults into a host buffer at
// the end of the pass, where the queries it used are also reset for the next.
class OcclusionQueries
{
public:
    // count queries, all reading zero until a pass writes them. An invalid
    // Device, a count below one, or a device without
    // Device::supportsOcclusionQueries() makes an invalid set, which a pass
    // ignores.
    OcclusionQueries(Device& device, int count);
    ~OcclusionQueries();

    OcclusionQueries(const OcclusionQueries&) = delete;
    OcclusionQueries& operator=(const OcclusionQueries&) = delete;

    bool isValid() const;
    int size() const;

    // Whether the GPU has finished the last frame that wrote any of these
    // queries, so read() would not wait. True on a set no pass has named yet.
    // False while that frame is still being recorded, which is before its
    // passes have reached the GPU at all.
    bool isComplete() const;

    // Copies results[i] = query (first + i) out, after waiting for the last
    // frame that wrote the set where it has been submitted and not finished.
    // A frame still being recorded is not waited for - it would never finish -
    // so a read inside it sees what the set held before. The copy is clamped
    // to the set.
    void read(std::span<std::uint64_t> results, int first = 0) const;

    // One query's result, by the same rule. Zero for an index the set lacks.
    std::uint64_t result(int index) const;

    // Internal, for the passes on each backend: the native objects, and the
    // hooks a pass calls as it begins and ends with this set named.
    void* nativeResults() const;
    void* nativeQueries() const;

    // A pass on `nativeRecording` names the set: the MTLCommandBuffer on
    // Metal, the CommandContext on D3D12 and Vulkan. Remembered so isComplete
    // and read know which submission to ask about; on Vulkan, also where the
    // pool's first reset is recorded, before the pass begins rendering.
    void notePass(void* nativeRecording);

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
