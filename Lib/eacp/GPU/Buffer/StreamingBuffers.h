#pragma once

#include "Buffer.h"

#include <eacp/Core/Utils/Containers.h>

#include <cstdint>

namespace eacp::GPU
{
// Storage for data rewritten every frame.
//
// Buffer::update does not synchronise against frames still in flight, so
// writing one buffer each tick tears the picture. This keeps one pool of
// buffers per frame that may be in flight and hands out a buffer from the
// current frame's pool, recycling a pool only once the frame that used it can
// no longer be on the GPU.
//
// Several writes in one frame get several buffers, which is the part a rotating
// single buffer gets wrong. A batching renderer flushes many times per frame
// through one shader - on every texture change, sampling change, scissor change
// and again at pass end - and each flush's draw is still queued in the command
// buffer when the next flush is written. One buffer per frame would have the
// second write land on top of geometry the first draw has not consumed yet.
//
// Pools grow on demand and never shrink, so steady state allocates nothing;
// Device::buffersCreated() is the number that says so.
//
// Which frame a write belongs to comes from Device's own counter, which Frame
// bumps. There is deliberately no advance() to call, and therefore none to
// forget - the same instinct as RenderPass::Participant. A forgotten advance
// would show up as tearing in whichever renderer forgot it, which is about the
// hardest kind of bug there is to attribute.
//
// This means a frame is what Frame says it is. Data streamed outside one - a
// CommandBuffer submitted on its own - stays on a single pool and keeps taking
// new buffers from it, so this is not the type for that; a plain Buffer is,
// since CommandBuffer::commit blocks and nothing is in flight after it.
class StreamingBuffers
{
public:
    explicit StreamingBuffers(BufferUsage usage);

    // Uploads bytes into a buffer no in-flight frame is reading, and returns it
    // for binding. The reference stays valid until this frame's pool comes
    // round again, which is framesInFlight frames later - long past the point
    // the draw that bound it was submitted.
    const Buffer& write(const void* data, std::size_t bytes);

    // How many GPU buffers exist across every pool. Flat in steady state; a
    // number that keeps climbing is the bug this type exists to prevent.
    int bufferCount() const;

    // Matches the deepest pipeline either backend runs: Metal's default
    // drawable pool is three, DXGI's present queue is two. GPUView can be set
    // to fewer, which costs one pool that never gets used rather than being
    // wrong - the error that matters is the other direction.
    static constexpr int framesInFlight = 3;

private:
    // One frame's worth of buffers. `used` is how many of them the frame has
    // taken so far, and `frame` is which frame that was - which is what lets a
    // second write in one frame (take the next buffer) be told apart from a
    // later frame reclaiming the pool (start again at the first).
    //
    // Held by owning pointer so the buffers keep their addresses: write()
    // hands back a reference the caller holds until its draw is submitted, and
    // the pool may well grow again before then.
    struct Pool
    {
        OwnedVector<Buffer> buffers;
        std::uint64_t frame = 0;
        int used = 0;
    };

    BufferUsage usage;
    Pool pools[framesInFlight];
};
} // namespace eacp::GPU
