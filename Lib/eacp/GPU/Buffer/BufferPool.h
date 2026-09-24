#pragma once

#include "Buffer.h"

#include <cstdint>
#include <deque>
#include <map>
#include <utility>

namespace eacp::GPU
{
class Device;

// Where Device::makeBuffer(bytes, usage) gets its storage, so that work
// recorded again and again - an inference step, a frame's compute - stops
// asking the device for fresh memory for every temporary each time round. A
// fresh buffer costs more than an allocation: on Metal its pages are zeroed and
// made resident before the command buffer that first uses it can run (about
// 75 ms of a 290 ms Stable Audio medium DiT step, for 1,587 temporaries), and
// on D3D12 each one is a CreateCommittedResource.
//
// Nothing to call: a Buffer from makeBuffer(bytes) gives its storage back here
// when it is destroyed, and a later makeBuffer of the same size and usage takes
// it again - but only once the GPU has finished every submission that could
// still be using it, which is everything submitted before the Buffer went and
// the one submitted next, the command buffer still being recorded when a
// temporary goes out of scope mid-encode. The one order that does not cover is
// two command buffers recorded at once and submitted out of order, the older
// after the newer, with a pooled buffer destroyed in between.
//
// So the contents of such a buffer are whatever was there, as the name
// "uninitialised" always allowed. Storage no makeBuffer asks for through a few
// submissions is freed, so the pool holds on to what the work is still using
// and not to what it has moved on from. Buffers made with data, and adopted
// memory, never come from here.
//
// One per Device, used from the Device's own thread.
class BufferPool
{
public:
    static BufferPool& of(Device& device);

    Buffer take(std::int64_t bytes, BufferUsage usage);

    // How many buffers the pool holds, waiting for the GPU or for reuse.
    int heldCount() const { return (int) (waiting.size() + available.size()); }

private:
    friend class Buffer;

    using Key = std::pair<std::int64_t, BufferUsage>;

    struct Waiting
    {
        std::uint64_t freeAfter = 0;
        Key key;
        Buffer buffer;
    };

    struct Available
    {
        std::uint64_t since = 0;
        Buffer buffer;
    };

    void give(Buffer storage, Key key);
    void promoteFinished();
    void freeUnused();

    static constexpr std::uint64_t submissionsKeptUnused = 2;

    Device* device = nullptr;
    std::uint64_t lastTrimmed = 0;
    std::deque<Waiting> waiting;
    std::multimap<Key, Available> available;
};
} // namespace eacp::GPU
