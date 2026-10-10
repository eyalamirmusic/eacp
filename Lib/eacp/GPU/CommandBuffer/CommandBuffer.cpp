#include "CommandBuffer.h"

#include "CopyRecorder.h"

// Portable CommandBuffer members. The platform backends own recording,
// submission and the fence; what builds on the public API alone lives here so
// it compiles once for every platform.

namespace eacp::GPU
{
void CommandBuffer::fill(const Buffer& buffer, std::uint8_t value)
{
    fill(BufferRange::of(buffer), value);
}

void CommandBuffer::update(Buffer& buffer,
                           const void* data,
                           std::int64_t bytes,
                           std::int64_t offset)
{
    // The whole of it: this command buffer's own wait, and then the write with
    // no second wait inside it. Buffer::update would order against the newest
    // submission, which is exactly what a caller reaching for this one has
    // already ordered against by hand and does not want to pay for again.
    wait();
    buffer.updateUnordered(data, bytes, offset);
}

void CommandBuffer::copy(const Texture& source, const Texture& destination)
{
    for (const auto& plan: planWholeTextureCopy(source, destination))
        recordCopy(nativeRecording(), plan);
}

void CommandBuffer::copy(const Texture& source,
                         const Texture& destination,
                         const TextureCopyRegion& region)
{
    if (auto plan = planTextureCopy(source, destination, region))
        recordCopy(nativeRecording(), *plan);
}

void CommandBuffer::copy(const Buffer& source, const Buffer& destination)
{
    copy(source, destination, 0, 0, source.size());
}

void CommandBuffer::copy(const Buffer& source,
                         const Buffer& destination,
                         std::int64_t sourceOffset,
                         std::int64_t destinationOffset,
                         std::int64_t bytes)
{
    if (auto plan = planBufferCopy(
            source, destination, sourceOffset, destinationOffset, bytes))
        recordCopy(nativeRecording(), *plan);
}

void CommandBuffer::copy(const Buffer& source,
                         const Texture& destination,
                         const BufferTextureCopy& layout)
{
    if (auto plan = planBufferTextureCopy(source, destination, layout))
        recordCopyToTexture(nativeRecording(), *plan);
}

void CommandBuffer::copy(const Texture& source,
                         const Buffer& destination,
                         const BufferTextureCopy& layout)
{
    if (auto plan = planBufferTextureCopy(destination, source, layout))
        recordCopyToBuffer(nativeRecording(), *plan);
}

void CommandBuffer::generateMipmaps(const Texture& texture)
{
    if (planMipGeneration(texture))
        recordMipGeneration(nativeRecording(), texture);
}
} // namespace eacp::GPU
