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

    // How many submissions storage nobody asks for again is kept through
    // before it is let go. Long enough to outlive one round of the work,
    // which is the thing a pool exists to serve: a sampling step submits
    // once, so two was enough for its temporaries to survive into the next
    // step, but a codec decode submits four to six times, so with two every
    // temporary it made was freed before the next decode asked for that size
    // - 181 buffers and 1.5 GB re-created per decode, for no reuse at all.
    // Peak memory is the same either way, measured: what the pool holds now
    // is exactly what was being freed and allocated again a moment later.
    static constexpr std::uint64_t submissionsKeptUnused = 64;

    // And no more than this much storage held unused, whatever the
    // submissions say. The submission rule alone cannot bound a pool: nothing
    // frees storage except a later take(), so a process that does its work and
    // then sits idle - a plugin between renders, a UI between frames - holds
    // whatever it last used for ever. Generating one medium clip left 6.0 GB
    // in the pool before this, which is not memory an idle app should keep.
    //
    // Two gigabytes is what the work here actually reuses (a decode turns over
    // about 1.5 GB), and holding more than it reuses buys nothing - so this is
    // a ceiling and not a target. A device too small to spare that gets a
    // quarter of what it recommends instead, which is the number that matters
    // on a 4 GB card and never binds on a large one.
    std::int64_t bytesKeptUnused() const;

    static constexpr std::int64_t bytesKeptUnusedCeiling = 2ll * 1024 * 1024 * 1024;

    // How many buffers the pool holds, waiting for the GPU or for reuse.
    int heldCount() const { return (int) (waiting.size() + available.size()); }

    std::int64_t heldBytes() const
    {
        auto total = std::int64_t {0};

        for (const auto& entry: waiting)
            total += entry.key.first;

        for (const auto& entry: available)
            total += entry.first.first;

        return total;
    }

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
    void freeOldestBeyondBudget();


    Device* device = nullptr;
    std::uint64_t lastTrimmed = 0;
    std::int64_t availableBytes = 0;
    mutable std::int64_t bound = 0;
    std::deque<Waiting> waiting;
    std::multimap<Key, Available> available;
};
} // namespace eacp::GPU
