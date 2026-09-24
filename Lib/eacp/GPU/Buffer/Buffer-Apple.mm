#import <Metal/Metal.h>

#include "Buffer.h"

#include "../Device/Device.h"

#include <eacp/Core/ObjC/ObjC.h>

#include <unistd.h>

namespace eacp::GPU
{
namespace
{
// Metal hands the deallocator whole pages, so the length the buffer is created
// with is the caller's count rounded up to the page its last byte lies in -
// which is mapped already. size() keeps reporting the count the caller gave.
std::int64_t roundedUpToPage(std::int64_t bytes)
{
    const auto page = Buffer::memoryPageSize();

    return ((bytes + page - 1) / page) * page;
}
} // namespace

struct Buffer::Native
{
    Native(Device& deviceToUse,
           const void* data,
           std::int64_t bytes,
           BufferUsage,
           BufferStorage)
        : device(&deviceToUse)
        , length(bytes)
    {
        auto metalDevice = (__bridge id<MTLDevice>) device->nativeDevice();

        if (metalDevice == nil || bytes <= 0)
            return;

        // Shared storage keeps the buffer CPU-visible, so read() is a memcpy and
        // a compute output needs no staging copy. The usage flag is a Metal no-op:
        // a plain MTLBuffer already serves as a vertex or a device storage buffer.
        //
        // So is BufferStorage: what it asks for on D3D12 - memory the CPU writes
        // in place and the GPU reads with no copy in between - is what every
        // buffer here already is, and update() below is already the memcpy it
        // buys. Metal has nothing to opt into and no second path to keep right.
        if (data != nullptr)
            buffer = [metalDevice newBufferWithBytes:data
                                              length:(NSUInteger) bytes
                                             options:MTLResourceStorageModeShared];
        else
            buffer = [metalDevice newBufferWithLength:(NSUInteger) bytes
                                              options:MTLResourceStorageModeShared];
    }

    // The zero-copy path. Metal takes host pages as a buffer outright when the
    // address is page-aligned, which is what makes a mapped weights file
    // readable by a kernel without a second copy of it existing anywhere.
    //
    // The deallocator is the release rather than this struct's destructor,
    // because it is Metal that knows when the memory is finished with: a
    // command buffer still in flight holds a reference to the MTLBuffer long
    // after the Buffer that made it has gone, and freeing the pages on the
    // Buffer's own destruction would pull them out from under it.
    Native(Device& deviceToUse, ExternalMemory memory, BufferUsage)
        : device(&deviceToUse)
        , length(memory.byteCount)
    {
        auto metalDevice = (__bridge id<MTLDevice>) device->nativeDevice();
        auto onReleased = std::move(memory.onReleased);

        if (metalDevice == nil || !Buffer::isPageAligned(memory))
        {
            length = 0;
            onReleased();

            return;
        }

        buffer = [metalDevice
            newBufferWithBytesNoCopy:memory.bytes
                              length:(NSUInteger) roundedUpToPage(memory.byteCount)
                             options:MTLResourceStorageModeShared
                         deallocator:^(void*, NSUInteger) { onReleased(); }];

        // Nothing adopted the memory, so nothing will ever call the block: the
        // caller is told here instead, and once.
        if (buffer.get() == nil)
        {
            length = 0;
            onReleased();

            return;
        }

        requestResidencyInBackground(metalDevice);
    }

    // Metal wires a no-copy buffer's pages the first time a command buffer uses
    // it, and for a mapped checkpoint that is half a second per nine gigabytes
    // spent inside the first dispatch. A residency set asks for it now, on a
    // background queue, so it overlaps whatever the caller does after loading
    // rather than the first thing it runs - and reads the pages in from disk on
    // the way when the file is not in the page cache. The set lives as long as
    // the buffer and keeps its pages resident, as the first use would have.
    void requestResidencyInBackground(id<MTLDevice> metalDevice)
    {
        if (@available(macOS 15.0, iOS 18.0, *))
        {
            auto descriptor = ObjC::Ptr<MTLResidencySetDescriptor> {};
            descriptor = [MTLResidencySetDescriptor new];

            id<MTLResidencySet> set =
                [metalDevice newResidencySetWithDescriptor:descriptor.get()
                                                     error:nil];

            if (set == nil)
                return;

            residency = (NSObject*) set;

            [set addAllocation:buffer.get()];
            [set commit];

            // Handed over unretained, so the set - and the buffer it holds -
            // go when this Native does rather than when the block is disposed
            // of; the destructor waits for the request instead.
            auto* unretainedSet = (__bridge void*) set;
            residencyRequest = dispatch_group_create();

            dispatch_group_async(
                residencyRequest,
                dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                ^{ [(__bridge id<MTLResidencySet>) unretainedSet requestResidency]; });
        }
    }

    ~Native()
    {
        if (residencyRequest == nullptr)
            return;

        dispatch_group_wait(residencyRequest, DISPATCH_TIME_FOREVER);
        dispatch_release(residencyRequest);
    }

    Native(const Native&) = delete;
    Native& operator=(const Native&) = delete;

    // The copy both update paths end in, so that each of them asserts the
    // owning thread once rather than once on the way through the other.
    void write(const void* data, std::int64_t bytes, std::int64_t offset)
    {
        auto metalBuffer = buffer.get();

        if (metalBuffer == nil || data == nullptr || bytes <= 0 || offset < 0
            || offset >= length)
            return;

        const auto available = length - offset;
        const auto count = bytes < available ? bytes : available;

        std::memcpy((char*) [metalBuffer contents] + offset, data, (std::size_t) count);
    }

    ObjC::Ptr<NSObject<MTLBuffer>> buffer;
    ObjC::Ptr<NSObject> residency;
    dispatch_group_t residencyRequest = nullptr;
    Device* device = nullptr;
    std::int64_t length = 0;
};

Buffer::Buffer(Device& device,
               const void* data,
               std::int64_t bytes,
               BufferUsage usage,
               BufferStorage storage)
    : impl(device, data, bytes, usage, storage)
{
    // Only the ones that got storage, so the count means GPU allocations rather
    // than calls - a zero-byte or device-less Buffer allocated nothing.
    if (isValid())
        device.noteBufferCreated();
}

Buffer::Buffer(Device& device, ExternalMemory memory, BufferUsage usage)
    : impl(device, std::move(memory), usage)
{
    if (isValid())
        device.noteBufferCreated();
}

// True of every Metal device there is, and false of a Device that never came up
// - which makes no buffers at all, adopted or otherwise.
//
// There is deliberately no copying fallback behind it: memory Metal declines
// leaves an invalid Buffer rather than a silent copy of a file the size of a
// model, which is the one outcome a caller reaching for this would not want to
// discover from a memory graph.
bool Buffer::canAdoptMemory(const Device& device)
{
    return device.isValid();
}

std::int64_t Buffer::memoryPageSize()
{
    return (std::int64_t) getpagesize();
}

std::int64_t Buffer::size() const
{
    return impl->length;
}

bool Buffer::isValid() const
{
    return impl->buffer.get() != nil;
}

void Buffer::read(void* dst, std::int64_t bytes, std::int64_t offset) const
{
    if (impl->device != nullptr)
        impl->device->assertOwningThread();

    if (bytes <= 0 || offset < 0 || offset >= impl->length)
        return;

    // Shared storage makes the copy itself a memcpy, but the kernel that filled
    // the buffer may still be running: commitAsync returns before the GPU has
    // done anything at all. Waiting for the newest submission waits for every
    // earlier one too, which is the same ordering the D3D12 read gets from the
    // queue's fence — and costs nothing once the work has landed.
    if (impl->device != nullptr)
        impl->device->waitForSubmittedWork();

    auto available = impl->length - offset;
    auto count = bytes < available ? bytes : available;

    if (auto metalBuffer = impl->buffer.get())
        std::memcpy(
            dst, (const char*) [metalBuffer contents] + offset, (std::size_t) count);
}

void Buffer::update(const void* data, std::int64_t bytes, std::int64_t offset)
{
    // Every Metal buffer here is shared storage, so the write below is a memcpy
    // into memory a kernel submitted earlier may still be writing. Waiting for
    // the newest submission is what puts the host's bytes after it, and it is
    // the same wait read() takes - see the rule on Buffer.h, and
    // updateUnordered for the frame loop, which cannot afford it.
    if (impl->device != nullptr)
    {
        impl->device->assertOwningThread();
        impl->device->waitForSubmittedWork();
    }

    impl->write(data, bytes, offset);
}

void Buffer::updateUnordered(const void* data, std::int64_t bytes, std::int64_t offset)
{
    if (impl->device != nullptr)
        impl->device->assertOwningThread();

    impl->write(data, bytes, offset);
}

void* Buffer::nativeBuffer() const
{
    return (__bridge void*) impl->buffer.get();
}

void* Buffer::nativeReadView() const
{
    return nullptr;
}

void* Buffer::nativeWriteView() const
{
    return nullptr;
}
} // namespace eacp::GPU
