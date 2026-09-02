#pragma once

#include "Buffer.h"

#include <eacp/Core/Utils/Containers.h>

#include <cstdint>

namespace eacp::GPU
{
// Storage for data rewritten every frame.
//
// Buffer::update does not synchronise against frames still in flight, so
// writing one buffer each tick tears the picture. This keeps one arena per
// frame that may be in flight and hands out slices of the current frame's
// arena, reclaiming an arena only once the frame that used it can no longer
// be on the GPU.
//
// Several writes in one frame get several slices, which is the part a rotating
// single buffer gets wrong. A batching renderer flushes many times per frame
// through one shader - on every texture change, sampling change, scissor change
// and again at pass end - and each flush's draw is still queued in the command
// buffer when the next flush is written. One buffer per frame, rewritten from
// the start each time, would have the second write land on top of geometry the
// first draw has not consumed yet. Slices of one buffer, each after the last,
// cannot.
//
// One arena rather than one buffer per write, and the difference is the whole
// cost model. A renderer that streams its geometry per draw - a game issuing a
// thousand draws a frame, each with its own vertices - makes a thousand writes
// a frame. As a thousand GPU buffers that is a thousand resources for the
// driver to make resident before every command buffer can be scheduled, and a
// thousand buffers each sized for the largest write that ever happened to land
// in its position, which keeps reallocating for as long as the draw order keeps
// changing. As slices of one arena it is one resource, a memcpy per write, and
// nothing allocated once the arena is as big as the frame. What a slice costs
// is what it is: its bytes, rounded up to an alignment.
//
// Arenas grow on demand and never shrink, so steady state allocates nothing;
// Device::buffersCreated() is the number that says so, and bufferCount() is
// framesInFlight once every pool is warm.
//
// Which frame a write belongs to comes from Device's own counter, which Frame
// bumps. There is deliberately no advance() to call, and therefore none to
// forget - the same instinct as RenderPass::Participant. A forgotten advance
// would show up as tearing in whichever renderer forgot it, which is about the
// hardest kind of bug there is to attribute.
//
// This means a frame is what Frame says it is. Data streamed outside one - a
// CommandBuffer submitted on its own - stays on a single pool and keeps taking
// slices from it, so this is not the type for that; a plain Buffer is, since
// CommandBuffer::commit blocks and nothing is in flight after it. Inside one
// long frame - a load screen redrawn many times before its tick ends - the
// arena simply grows to hold every write of it, which is bytes rather than
// resources, and what it grew to is kept for the next frame that needs it.
class StreamingBuffers
{
public:
    explicit StreamingBuffers(BufferUsage usage);

    // Copies bytes into a slice of an arena no in-flight frame is reading, and
    // returns the slice for binding. The range stays valid until this frame's
    // pool comes round again, which is framesInFlight frames later - long past
    // the point the draw that bound it was submitted.
    //
    // Slices start on alignment-byte boundaries, so two writes in one frame
    // never share one; what that costs is at most alignment - 1 bytes a write.
    // A write of zero bytes takes no room and comes back as an empty range at
    // the arena's current position, so a caller that binds whatever it wrote
    // still binds something real.
    BufferRange write(const void* data, std::size_t bytes);

    // How many GPU buffers exist across every pool. framesInFlight once each
    // pool is warm, and more only during a frame that outgrew its arena - the
    // next time that pool comes round it is one arena again. A number that
    // keeps climbing is the bug this type exists to prevent.
    //
    // What "warm" means, for anyone watching Device::buffersCreated(): a frame
    // larger than any before it grows its pool, and the fold when that pool
    // next comes round may allocate once more, the one arena sized for the
    // whole of that frame. So the count settles two periods after a change of
    // load, not one - see StreamingStress, whose table shows exactly that.
    int bufferCount() const;

    // The bytes those buffers reserve, across every pool. Grows to the largest
    // frame any pool has seen and stays there.
    std::size_t bytesReserved() const;

    // Matches the deepest pipeline either backend runs: Metal's default
    // drawable pool is three, DXGI's present queue is two. GPUView can be set
    // to fewer, which costs one pool that never gets used rather than being
    // wrong - the error that matters is the other direction.
    static constexpr int framesInFlight = 3;

    // Where a slice may start. The strictest offset either API asks of any
    // buffer bind is the 256 of a constant buffer view, on D3D12 and on Metal
    // alike - vertex and index binds want far less. Paying the larger one
    // makes every slice bindable anywhere, and against arenas measured in
    // megabytes the padding does not register.
    static constexpr std::size_t alignment = 256;

private:
    // One frame's worth of storage. In steady state `arenas` holds exactly one
    // buffer and `used` is the cursor into it; `frame` is which frame filled
    // it, which is what lets a second write in one frame (carry on from the
    // cursor) be told apart from a later frame reclaiming the pool (start
    // again at zero).
    //
    // A frame that outgrows its arena cannot have it replaced under the draws
    // already recorded into it, so a second arena is appended for the rest of
    // the frame and the two are folded into one when the pool next comes
    // round - see StreamingBuffers.cpp. `streamed` is what the current frame
    // has taken across all of them, padding included, and `highWater` the most
    // any frame has, which is what the folded arena is sized from.
    //
    // Held by owning pointer so the arenas keep their addresses: write() hands
    // back a range pointing at one, which the caller holds until its draw is
    // submitted, and the pool may well grow again before then.
    struct Pool
    {
        OwnedVector<Buffer> arenas;
        std::uint64_t frame = 0;
        std::size_t used = 0;
        std::size_t streamed = 0;
        std::size_t highWater = 0;
    };

    void beginFrame(Pool& pool, std::uint64_t frame);
    Buffer& arenaFor(Pool& pool, std::size_t bytes);

    BufferUsage usage;
    Pool pools[framesInFlight];
};
} // namespace eacp::GPU
