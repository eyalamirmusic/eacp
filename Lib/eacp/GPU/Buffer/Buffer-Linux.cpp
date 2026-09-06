#include "Buffer.h"

#include "../Device/Device.h"
#include "../Vulkan/VulkanTypes.h"

#include <cstring>

// Linux/Vulkan backend. A BufferStorage::Device buffer is device-local memory
// filled by a staged copy: a memcpy into the recording's upload arena and a
// vkCmdCopyBuffer onto whatever recording is already open, so a batching
// renderer that builds a buffer per flush pays one submission for the frame
// rather than one per buffer. read() copies into a pooled readback buffer and
// waits on the context's timeline, which preserves the contract that a read
// after commit() sees the kernel's output.
//
// A BufferStorage::Streaming buffer is the other shape entirely: host-visible
// coherent memory, mapped once and kept mapped, bound as vertex, index or
// storage data in place. Every write is a memcpy through that mapping and puts
// nothing on a command buffer - no staging chunk, no copy, no barrier either
// side of it. Correctness comes from the caller instead: see BufferStorage in
// Buffer.h, and StreamingBuffers, which is the caller that has it.

namespace eacp::GPU
{
namespace
{
// Every usage bit at once, which is a deliberate difference from the two other
// backends rather than laziness. Vulkan needs a buffer's uses declared when it
// is created and refuses one that is bound any other way; eacp's BufferUsage is
// advisory on Metal and picks resource flags on D3D12, so nothing above this
// layer promises that a Vertex buffer is never read by a kernel or that a
// Storage buffer never feeds drawIndexed - and StreamingBuffers hands out
// ranges of one arena for whichever of those the caller wants. Declaring the
// union costs nothing on any driver: usage bits pick a memory type and a
// layout, and every one of these lands on the same buffer memory.
constexpr VkBufferUsageFlags vulkanBufferUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
    | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
    | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
} // namespace

struct Buffer::Native
{
    Native(Device& device,
           const void* data,
           std::size_t bytes,
           BufferUsage usage,
           BufferStorage storage)
        : context(getVulkanContext(device))
    {
        bufferData.size = bytes;

        if (!context.isValid() || bytes == 0)
            return;

        // A Storage buffer keeps device storage whatever was asked for, which
        // is the contract BufferStorage states. Vulkan would allow a
        // host-visible storage buffer where D3D12 cannot, but a kernel writing
        // across the bus is the trade this option exists to avoid, not one to
        // make silently.
        if (storage == BufferStorage::Streaming && usage != BufferUsage::Storage)
            if (mapStreamingStorage(data, bytes))
                return;

        if (!makeDeviceBuffer(bytes))
            return;

        // A buffer whose initial data never reached it is not a buffer, and
        // isValid() is how the caller finds out rather than drawing from
        // whatever the heap happened to hold.
        if (data != nullptr && !stage(data, bytes))
            release();
    }

    // Handed to the context rather than destroyed here. A buffer is routinely
    // replaced mid-frame - every ShaderProgram setInstances/setVertices makes a
    // new one - and the command buffer still recording names the old handle.
    ~Native() { release(); }

    void release()
    {
        context.deferReleaseBuffer(bufferData.buffer, bufferData.allocation);

        bufferData.buffer = VK_NULL_HANDLE;
        bufferData.allocation = nullptr;
        bufferData.mapped = nullptr;
    }

    bool makeDeviceBuffer(std::size_t bytes)
    {
        VkBufferCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = vulkanBufferUsage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        // No host-access flag, which is what tells VMA to put this in device
        // memory. Sub-allocated out of a larger block rather than given a
        // VkDeviceMemory of its own, which is the whole reason the allocator is
        // here - maxMemoryAllocationCount is commonly 4096.
        VmaAllocationCreateInfo allocationInfo = {};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;

        return vmaCreateBuffer(context.getAllocator(),
                               &info,
                               &allocationInfo,
                               &bufferData.buffer,
                               &bufferData.allocation,
                               nullptr)
               == VK_SUCCESS;
    }

    // One host-visible coherent buffer, mapped for good. False when either step
    // fails, which leaves the caller to fall through to the device path rather
    // than hand back a buffer that never got storage.
    bool mapStreamingStorage(const void* data, std::size_t bytes)
    {
        VkBufferCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = vulkanBufferUsage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocationInfo = {};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocationInfo.flags =
            VMA_ALLOCATION_CREATE_MAPPED_BIT
            | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        allocationInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                       | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        VmaAllocationInfo result = {};

        if (vmaCreateBuffer(context.getAllocator(),
                            &info,
                            &allocationInfo,
                            &bufferData.buffer,
                            &bufferData.allocation,
                            &result)
            != VK_SUCCESS)
            return false;

        bufferData.mapped = static_cast<std::byte*>(result.pMappedData);

        if (bufferData.mapped == nullptr)
        {
            release();
            return false;
        }

        if (data != nullptr)
            std::memcpy(bufferData.mapped, data, bytes);

        return true;
    }

    // The copy that fills the buffer from CPU bytes, and the whole of what makes
    // a buffer cheap enough to create per draw. Two things, of the same order of
    // cost: the staging bytes are bump-allocated from the recording's upload
    // arena instead of a buffer made for this one copy, and the copy goes onto
    // whatever recording is already open instead of one acquired and submitted
    // for it alone.
    //
    // Recording onto the open command buffer rather than ahead of it is also
    // what keeps it correct: the copy lands in order, before the dispatch that
    // wanted the bytes, so two flushes of the same program in one frame each
    // read what they were given.
    bool stage(const void* data, std::size_t bytes, std::size_t destination = 0)
    {
        if (bufferData.buffer == VK_NULL_HANDLE)
            return false;

        auto* commands = context.getOpenRecording();
        const auto ownsRecording = commands == nullptr;

        if (ownsRecording)
            commands = context.acquire();

        if (commands == nullptr)
            return false;

        const auto copied = copyInto(*commands, data, bytes, destination);

        if (!ownsRecording)
            return copied;

        if (copied)
            context.submit(commands);
        else
            context.discard(commands);

        return copied;
    }

    bool copyInto(CommandContext& commands,
                  const void* data,
                  std::size_t bytes,
                  std::size_t destination)
    {
        auto source = context.allocateUpload(commands, bytes);

        if (!source.isValid())
            return false;

        std::memcpy(source.mapped, data, bytes);

        transitionForUse(commands, bufferData, bufferTransferWrite);

        VkBufferCopy region = {};
        region.srcOffset = source.offset;
        region.dstOffset = static_cast<VkDeviceSize>(destination);
        region.size = static_cast<VkDeviceSize>(bytes);

        vkCmdCopyBuffer(
            commands.buffer, source.buffer, bufferData.buffer, 1, &region);

        return true;
    }

    // The Device's context, held for the buffer's lifetime: read(), update() and
    // the deferred release all belong to the timeline this memory was allocated
    // against, and a buffer never moves between Devices.
    VulkanContext& context;

    // Mutable because the use tracking advances inside the const read(): the
    // copy into the readback buffer is a use like any other.
    mutable VulkanBufferData bufferData;
};

Buffer::Buffer(Device& device,
               const void* data,
               std::size_t bytes,
               BufferUsage usage,
               BufferStorage storage)
    : impl(device, data, bytes, usage, storage)
{
    // Only the ones that got storage, so the count means GPU allocations rather
    // than calls - a zero-byte or device-less Buffer allocated nothing.
    if (isValid())
        device.noteBufferCreated();
}

std::size_t Buffer::size() const
{
    return impl->bufferData.size;
}

bool Buffer::isValid() const
{
    return impl->bufferData.buffer != VK_NULL_HANDLE;
}

void Buffer::read(void* dst, std::size_t bytes, std::size_t offset) const
{
    if (impl->bufferData.buffer == VK_NULL_HANDLE || offset >= impl->bufferData.size)
        return;

    const auto available = impl->bufferData.size - offset;
    const auto count = bytes < available ? bytes : available;

    // Host storage reads straight back out of the mapping, as Metal's shared
    // buffers always have. Nothing on the GPU writes those bytes, so there is
    // no work to wait for and no readback copy to make.
    if (impl->bufferData.mapped != nullptr)
    {
        std::memcpy(dst, impl->bufferData.mapped + offset, count);
        return;
    }

    auto& context = impl->context;
    auto* commands = context.acquire();

    if (commands == nullptr)
        return;

    // Out of the pool, for the reason the upload side takes one: a read repeats
    // every run at the same size. The pool owns it, and it stays valid until
    // the value this submission signals has passed.
    std::byte* mapped = nullptr;
    auto staging = context.acquireReadbackBuffer(*commands, count, mapped);

    if (staging == VK_NULL_HANDLE || mapped == nullptr)
    {
        context.discard(commands);
        return;
    }

    // This recording lives beside whatever recording is open, and the buffer's
    // use tracking is per recording: stamping this one on the buffer would make
    // the open recording's next use of it look like a first use, free of the
    // barrier it may still owe against a write it recorded earlier. So the
    // tracking goes back the way it was once the copy is submitted. This
    // recording ends with the global barrier like every other, which is what
    // makes the copy visible to whatever follows it.
    const auto trackedRecording = impl->bufferData.recordingId;
    const auto trackedUse = impl->bufferData.use;

    transitionForUse(*commands, impl->bufferData, bufferTransferRead);

    VkBufferCopy region = {};
    region.srcOffset = static_cast<VkDeviceSize>(offset);
    region.size = static_cast<VkDeviceSize>(count);

    vkCmdCopyBuffer(commands->buffer, impl->bufferData.buffer, staging, 1, &region);

    // The copy was enqueued on the same queue as the writes, so waiting for
    // this submission also waits for them.
    context.waitFor(context.submit(commands));

    impl->bufferData.recordingId = trackedRecording;
    impl->bufferData.use = trackedUse;

    // Coherent memory, so what the GPU wrote is what the CPU reads with no
    // invalidate in between - see VulkanContext::makeHostBuffer.
    std::memcpy(dst, mapped, count);
}

void Buffer::update(const void* data, std::size_t bytes, std::size_t offset)
{
    if (impl->bufferData.buffer == VK_NULL_HANDLE || data == nullptr || bytes == 0
        || offset >= impl->bufferData.size)
        return;

    if (!impl->context.isValid())
        return;

    const auto available = impl->bufferData.size - offset;
    const auto count = bytes < available ? bytes : available;

    // The whole of a streamed write. No recording is touched, so nothing orders
    // it against the dispatches already recorded and nothing needs to: what
    // makes it safe is that the caller does not write bytes an in-flight frame
    // is still reading, which is the contract BufferStorage::Streaming states
    // and StreamingBuffers keeps.
    if (impl->bufferData.mapped != nullptr)
    {
        std::memcpy(impl->bufferData.mapped + offset, data, count);
        return;
    }

    impl->stage(data, count, offset);
}

void* Buffer::nativeBuffer() const
{
    return &impl->bufferData;
}

// Both directions are the same handle here, as on D3D12: a GLSL storage buffer
// is one std430 block whether the kernel reads it or writes it, and the
// descriptor type does not change with the direction.
void* Buffer::nativeReadView() const
{
    return &impl->bufferData;
}

void* Buffer::nativeWriteView() const
{
    return &impl->bufferData;
}
} // namespace eacp::GPU
