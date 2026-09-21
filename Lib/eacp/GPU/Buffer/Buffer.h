#pragma once

#include "../Common.h"

#include <cstdint>

namespace eacp::GPU
{
class Device;

// What a buffer is bound as. A Vertex buffer feeds the vertex stage; an Index
// buffer feeds drawIndexed; a Storage buffer is read/written by a compute
// kernel and can be read back to the CPU. On Metal all are plain MTLBuffers;
// on D3D12 the usage picks the resource flags (a Storage buffer allows
// unordered access so a kernel can write it).
enum class BufferUsage
{
    Vertex,
    Index,
    Storage
};

// Where a buffer's storage lives, which is a statement about who writes it
// rather than about what it is bound as.
//
// Device is the default and what an app's own geometry wants: memory the GPU
// reads out of its own heap, filled through a staged copy. Streaming says the
// CPU rewrites these bytes every frame and the GPU only ever reads them, which
// buys a very different deal on D3D12 - the resource lives on the UPLOAD heap,
// stays mapped for its whole life, and is bound as vertex, index or constant
// data straight out of that mapping. A write is then one memcpy and records
// nothing: no staging chunk, no CopyBufferRegion, and no pair of barriers
// around it, since an upload-heap resource is permanently in GENERIC_READ. A
// renderer streaming hundreds of ranges a frame pays hundreds of barriers and
// copy commands for the Device answer and none at all for this one.
//
// What it costs is that the memory is CPU-visible and write-combined, so the
// GPU reads it across the bus rather than out of its own heap, and that
// nothing may rewrite a byte the GPU has not finished reading - there is no
// copy in the command stream for the write to be ordered behind. That second
// one is StreamingBuffers' whole contract, which is why this is the storage
// its arenas ask for and not something a long-lived buffer should reach for.
//
// On Metal it says nothing new: every eacp buffer is already a shared-storage
// MTLBuffer, and update() there already is the memcpy this asks for.
//
// A Storage buffer keeps device storage whatever is asked here. A kernel
// writing through a UAV is precisely what an upload heap cannot do, and a
// creation that quietly failed would be far worse than paying for the copy.
enum class BufferStorage
{
    Device,
    Streaming
};

// The width of the indices in an Index buffer, told to drawIndexed.
enum class IndexFormat
{
    UInt16,
    UInt32
};

// Memory the caller already owns, handed to a Buffer to be used where it lies
// rather than copied into one. Device::makeBufferOverMemory takes one; the
// tensors of a mapped weights file are what it is for, where a copy would mean
// a second few gigabytes and the time to write them.
//
// bytes must sit on Buffer::memoryPageSize(), because that is what the backend
// that can adopt memory asks of it, and a descriptor off that grid makes an
// invalid Buffer rather than a quietly copied one - so the contract is the same
// everywhere and code written on one backend keeps working on the next. An
// eacp::MemoryMappedFile of a whole file is already on the grid.
//
// byteCount is free of that: the buffer covers whole pages underneath, and the
// page the last byte lies in is already mapped, so a length that stops
// mid-page costs nothing and no range can be built past what the caller owns -
// size() is the count given here rather than the rounded one.
//
// What that rounding does mean is that the GPU is handed up to
// memoryPageSize() - 1 bytes past byteCount, inside the page the last byte
// already lies in. Nothing eacp encodes reaches them, since every bound range
// is clamped to size(); a caller whose last page holds something it would
// rather the GPU could not address should map or allocate that tail itself.
//
// onReleased is called once, when the memory is no longer the buffer's: when
// the Buffer is destroyed where the memory was adopted, and as soon as the copy
// has been taken where it was not. It is where a caller that mapped or
// allocated the block lets go of it, and defaults to doing nothing, which is
// right for memory that outlives the buffer anyway.
struct ExternalMemory
{
    void* bytes = nullptr;
    std::int64_t byteCount = 0;
    Callback onReleased = [] {};
};

// RAII wrapper around a GPU buffer (MTLBuffer on Metal). Create via
// Device::makeBuffer. Pass null data with a byte count to allocate an
// uninitialised buffer (e.g. a compute output target).
//
// Byte counts and offsets are std::int64_t throughout, because a single buffer
// is routinely past what an int holds: a language model's weight shard is
// gigabytes, and a batch of logits reaches two of them at a few thousand rows.
// An int count wrapped there silently, allocating something small and negative
// instead of failing. Signed rather than std::size_t so that a negative offset
// arriving from a caller's own arithmetic stays negative and the guards that
// reject it keep working, and so that mixing a count with the int element
// counts the rest of this API uses needs no cast in either direction.
class Buffer
{
public:
    Buffer(Device& device,
           const void* data,
           std::int64_t bytes,
           BufferUsage usage = BufferUsage::Vertex,
           BufferStorage storage = BufferStorage::Device);

    // The zero-copy sibling: a buffer over memory the caller owns, adopted
    // rather than copied where the backend can do that and copied where it
    // cannot. See ExternalMemory for the alignment contract and
    // canAdoptMemory for which of the two this device does.
    Buffer(Device& device,
           ExternalMemory memory,
           BufferUsage usage = BufferUsage::Storage);

    // Whether a buffer over caller-owned memory shares that memory rather than
    // copying it. True on a valid Metal device, where a shared-storage
    // MTLBuffer can be built straight over page-aligned host pages; false on
    // D3D12 and Vulkan, whose device heaps are not host memory and where the
    // same call copies, and false on a device that never came up, which makes
    // no buffers of any kind.
    //
    // Worth asking before mapping a file the size of a model: where this is
    // false the bytes are paid for twice while the buffer is created, and a
    // caller with a choice may prefer to stream them in a piece at a time.
    //
    // Where it is true there is no copying path to fall back on: memory the
    // driver declines to adopt yields an invalid Buffer, which is the same
    // answer isPageAligned refusing the descriptor gives, and is what makes
    // isValid() the one thing to check after the call.
    static bool canAdoptMemory(const Device& device);

    // The grid an ExternalMemory address has to sit on - the host page size,
    // which is what Metal's no-copy buffers require of the pointer they are
    // handed.
    static std::int64_t memoryPageSize();

    // Whether a descriptor is one a Buffer can be made over at all, which every
    // backend asks before it either adopts or copies. Worth calling at the
    // point the memory is allocated, where the answer can still be acted on.
    static bool isPageAligned(const ExternalMemory& memory)
    {
        const auto page = (std::uintptr_t) memoryPageSize();

        if (memory.bytes == nullptr || memory.byteCount <= 0 || page == 0)
            return false;

        return reinterpret_cast<std::uintptr_t>(memory.bytes) % page == 0;
    }

    std::int64_t size() const;
    bool isValid() const;

    // Copies bytes back from the buffer into dst, starting at offset bytes
    // into the buffer. Valid once the command buffer that wrote it has
    // committed (CommandBuffer::commit blocks until then). The copy is
    // clamped to the buffer's end; an offset past it reads nothing.
    //
    // Committed is also the rule for bytes the CPU wrote: a construction or
    // update that happened while a Frame was recording put its copy on that
    // frame's list, so it has not reached the buffer until the frame ends.
    // Reading inside the frame that filled it reads what was there before.
    //
    // A BufferStorage::Streaming buffer is the exception both backends share:
    // its bytes live in memory the CPU wrote directly and no GPU work ever
    // writes, so a read is a memcpy back out of them and sees the newest
    // update rather than the last committed one. It is a debugging and test
    // affordance rather than a frame-loop one - the memory is write-combined
    // on D3D12, and reading it is far slower than writing it.
    void read(void* dst, std::int64_t bytes, std::int64_t offset = 0) const;

    // Overwrites part of the buffer's contents from the CPU, starting at
    // offset bytes into the buffer — the per-frame path for dynamic
    // geometry, reusing the GPU resource instead of allocating a new one.
    // The copy is clamped to the buffer's end; a no-op on an invalid buffer,
    // null data or an offset past the end.
    //
    // The write is ordered after everything submitted to this device before
    // the call, exactly as read() is, so a kernel still writing these bytes
    // has finished before the host's arrive and the host's are what stay. That
    // costs a wait only where the write is a bare memcpy into memory the GPU
    // can see: on Metal, whose buffers are all shared storage, and on a
    // host-mapped BufferStorage::Streaming buffer anywhere. A device-storage
    // write on D3D12 and Vulkan is a copy recorded into the command stream,
    // which the stream itself already orders, and waits for nothing. What is
    // *not* ordered is the other direction: commands encoded after the call
    // see the new contents, and commands already recorded but not yet
    // submitted are not held back.
    //
    // That wait is new, and it is a cost every existing caller now pays: an
    // update that used to be a bare memcpy on Metal is a memcpy behind a wait
    // for the newest submission. Code that was already right by construction —
    // a per-frame write, or one after a wait of its own — gets its old cost
    // back by asking for updateUnordered below by name, which is what the
    // renderers and StreamingBuffers in this tree were changed to do.
    void update(const void* data, std::int64_t bytes, std::int64_t offset = 0);

    // The same write with that wait given up: the caller states that no work
    // the GPU still has in hand reads or writes these bytes, and the copy
    // happens now.
    //
    // There are two ways to be able to say that. The first is the per-frame
    // path and the reason BufferStorage::Streaming exists: a renderer rewriting
    // its geometry every tick cannot afford a wait for the newest submission in
    // the middle of a frame — that is the CPU and the GPU taking turns instead
    // of overlapping — so it buys the ordering some other way. StreamingBuffers
    // is that other way, and never hands out bytes from an arena a frame still
    // in flight was drawn from.
    //
    // The second is a caller that has ordered by hand and knows more than a
    // Buffer can: it waited on the command buffer that wrote these bytes, or it
    // read them back, or it is a step-by-step loop where the writer finished
    // long ago and only a later, unrelated submission is still running.
    // CommandBuffer::update is that case with the wait built in and scoped to
    // one command buffer, and is the better call where the writer is known.
    //
    // On backends where a device-storage write is a recorded copy there is
    // nothing to skip, and this is update() under another name.
    void updateUnordered(const void* data,
                         std::int64_t bytes,
                         std::int64_t offset = 0);

    // Opaque native handle for cross-translation-unit use by other GPU types.
    void* nativeBuffer() const;

    // The read and write handles a compute pass binds on D3D12 (the same
    // handle as nativeBuffer; the pass binds by GPU address and direction).
    // Null on Metal, where the buffer is bound directly.
    void* nativeReadView() const;
    void* nativeWriteView() const;

private:
    struct Native;
    Pimpl<Native> impl;

    // The one backend that wraps another - Linux's composite Device, which is
    // OpenGL for render and Vulkan for compute (plan.md D11) - has to reach the
    // backend behind a Buffer a pass was handed, since the native handle it
    // would otherwise go by is the one of whichever side asked. Declared here
    // on every platform and defined only in Buffer-Linux.cpp.
    friend struct BufferBackend& getBufferBackend(const Buffer& buffer);
};

// A contiguous slice of one Buffer: where it starts and how long it is, in
// bytes from the buffer's beginning.
//
// This is what a sub-allocator hands out and what a bind can take. A
// StreamingBuffers write comes back as one of these rather than as a whole
// buffer, and RenderPass::setVertexBuffer and drawIndexed accept one directly,
// which is what lets a frame's worth of geometry share a single GPU resource
// instead of being one resource per draw. Nothing about the buffer changes:
// the range only says which part of it a draw should read from.
//
// Holds a pointer, not ownership. Whoever handed the range out owns the buffer
// and says how long the range stays valid - a streamed one until its pool
// comes round again, a range over an app's own buffer as long as the app
// keeps the buffer.
//
// A vertex or index range starts anywhere a word does. A storage-buffer bind is
// the one that asks for more: its offset must be a multiple of
// Device::storageBufferOffsetAlignment(), four on Metal and D3D12 and the
// device's own limit on Vulkan, so a range meant for a kernel's slot is rounded
// to that rather than to the element size.
struct BufferRange
{
    const Buffer* buffer = nullptr;
    std::int64_t offset = 0;
    std::int64_t bytes = 0;

    // The whole of a buffer, for the calls that take a range when what a
    // caller has is a buffer it means to bind from the start.
    static BufferRange of(const Buffer& whole) { return {&whole, 0, whole.size()}; }

    // False for a default-constructed range and for one over a buffer that
    // never got storage, which is the same test a bind makes before encoding.
    bool isValid() const { return buffer != nullptr && buffer->isValid(); }
};
} // namespace eacp::GPU
