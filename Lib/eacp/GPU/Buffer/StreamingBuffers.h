#pragma once

#include "Buffer.h"

#include <eacp/Core/Utils/Containers.h>

#include <cstdint>

namespace eacp::GPU
{
// Per-frame storage for data rewritten every frame, giving each write a buffer
// no in-flight frame is reading. Frames come from Device's counter, which Frame
// bumps: outside a Frame everything lands in one pool, so use a plain Buffer.
class StreamingBuffers
{
public:
    explicit StreamingBuffers(BufferUsage usage);

    // The returned reference stays valid until this pool comes round again,
    // framesInFlight frames later.
    const Buffer& write(const void* data, std::size_t bytes);

    // Total GPU buffers across every pool; flat in steady state.
    int bufferCount() const;

    // The deepest pipeline either backend runs: Metal's drawable pool is three,
    // DXGI's present queue two. Overshooting only wastes an unused pool.
    static constexpr int framesInFlight = 3;

private:
    // Buffers are held by owning pointer so their addresses survive pool growth
    // while a caller holds a write() reference.
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
