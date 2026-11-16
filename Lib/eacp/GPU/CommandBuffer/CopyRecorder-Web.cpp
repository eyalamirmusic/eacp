#include "CopyRecorder.h"

#include "../WebGPU/WebGPUContext.h"

#include <eacp/Core/Utils/Logging.h>

namespace eacp::GPU
{
namespace
{
constexpr std::uint32_t copyRowAlignment = 256;

// The recording a copy may go on: none while a pass is open, since WebGPU
// records copies only between passes.
WebGPURecording* recordingOutsidePasses(void* recording)
{
    auto* commands = static_cast<WebGPURecording*>(recording);

    if (commands == nullptr || commands->encoder == nullptr)
        return nullptr;

    if (commands->passOpen)
    {
        LOG("eacp: a copy while a pass is open is refused; end the pass first");
        return nullptr;
    }

    return commands;
}

WebGPUTextureData* textureDataOf(const Texture& texture)
{
    auto* data = static_cast<WebGPUTextureData*>(texture.nativeTexture());
    return data != nullptr && data->isValid() ? data : nullptr;
}

WebGPUBufferData* bufferDataOf(const Buffer& buffer)
{
    auto* data = static_cast<WebGPUBufferData*>(buffer.nativeBuffer());
    return data != nullptr && data->buffer != nullptr ? data : nullptr;
}

wgpu::TexelCopyTextureInfo
    texelCopyOf(const WebGPUTextureData& data, int level, int x, int y, int z)
{
    auto info = wgpu::TexelCopyTextureInfo {};
    info.texture = data.texture;
    info.mipLevel = static_cast<std::uint32_t>(level);
    info.origin = {static_cast<std::uint32_t>(x),
                   static_cast<std::uint32_t>(y),
                   static_cast<std::uint32_t>(z)};
    return info;
}

// One copy where the rows sit on WebGPU's 256-byte grid, one per row where
// they do not: a single-row copy takes any row length.
template <typename Copy>
void copyRows(const BufferTextureCopyPlan& plan, Copy&& copy)
{
    const auto bytesPerRow = static_cast<std::uint32_t>(plan.bytesPerRow);

    if (bytesPerRow % copyRowAlignment == 0)
    {
        auto layout = wgpu::TexelCopyBufferLayout {};
        layout.offset = static_cast<std::uint64_t>(plan.bufferOffset);
        layout.bytesPerRow = bytesPerRow;
        layout.rowsPerImage = static_cast<std::uint32_t>(plan.height);
        copy(layout, 0, plan.height);
        return;
    }

    for (auto row = 0; row < plan.height; ++row)
    {
        auto layout = wgpu::TexelCopyBufferLayout {};
        layout.offset = static_cast<std::uint64_t>(plan.bufferOffset)
                        + static_cast<std::uint64_t>(row) * bytesPerRow;
        copy(layout, row, 1);
    }
}
} // namespace

void recordCopy(void* recording, const TextureCopyPlan& plan)
{
    auto* commands = recordingOutsidePasses(recording);

    if (commands == nullptr || plan.source == nullptr || plan.destination == nullptr)
        return;

    auto* source = textureDataOf(*plan.source);
    auto* destination = textureDataOf(*plan.destination);

    if (source == nullptr || destination == nullptr)
        return;

    const auto sourceVolume = source->kind == TextureType::ThreeD;
    const auto destinationVolume = destination->kind == TextureType::ThreeD;

    const auto from = texelCopyOf(*source,
                                  plan.sourceLevel,
                                  plan.sourceX,
                                  plan.sourceY,
                                  sourceVolume ? plan.sourceZ : plan.sourceSlice);
    const auto to =
        texelCopyOf(*destination,
                    plan.destinationLevel,
                    plan.destinationX,
                    plan.destinationY,
                    destinationVolume ? plan.destinationZ : plan.destinationSlice);

    const auto layers = sourceVolume ? plan.depth : plan.sliceCount;
    const auto size = wgpu::Extent3D {static_cast<std::uint32_t>(plan.width),
                                      static_cast<std::uint32_t>(plan.height),
                                      static_cast<std::uint32_t>(layers)};

    commands->encoder.CopyTextureToTexture(&from, &to, &size);
}

void recordCopy(void* recording, const BufferCopyPlan& plan)
{
    auto* commands = recordingOutsidePasses(recording);

    if (commands == nullptr || plan.source == nullptr || plan.destination == nullptr)
        return;

    auto* source = bufferDataOf(*plan.source);
    auto* destination = bufferDataOf(*plan.destination);

    if (source == nullptr || destination == nullptr)
        return;

    if (plan.sourceOffset % 4 != 0 || plan.destinationOffset % 4 != 0
        || plan.bytes % 4 != 0)
    {
        LOG("WebGPU: a buffer copy off the four-byte grid is refused");
        return;
    }

    commands->encoder.CopyBufferToBuffer(
        source->buffer,
        static_cast<std::uint64_t>(plan.sourceOffset),
        destination->buffer,
        static_cast<std::uint64_t>(plan.destinationOffset),
        static_cast<std::uint64_t>(plan.bytes));
}

void recordCopyToTexture(void* recording, const BufferTextureCopyPlan& plan)
{
    auto* commands = recordingOutsidePasses(recording);

    if (commands == nullptr || plan.buffer == nullptr || plan.texture == nullptr)
        return;

    auto* buffer = bufferDataOf(*plan.buffer);
    auto* texture = textureDataOf(*plan.texture);

    if (buffer == nullptr || texture == nullptr)
        return;

    copyRows(plan,
             [&](const wgpu::TexelCopyBufferLayout& layout, int row, int rows)
             {
                 auto source = wgpu::TexelCopyBufferInfo {};
                 source.layout = layout;
                 source.buffer = buffer->buffer;

                 const auto destination = texelCopyOf(
                     *texture, plan.level, plan.x, plan.y + row, plan.slice);
                 const auto size =
                     wgpu::Extent3D {static_cast<std::uint32_t>(plan.width),
                                     static_cast<std::uint32_t>(rows),
                                     1};

                 commands->encoder.CopyBufferToTexture(&source, &destination, &size);
             });
}

void recordCopyToBuffer(void* recording, const BufferTextureCopyPlan& plan)
{
    auto* commands = recordingOutsidePasses(recording);

    if (commands == nullptr || plan.buffer == nullptr || plan.texture == nullptr)
        return;

    auto* buffer = bufferDataOf(*plan.buffer);
    auto* texture = textureDataOf(*plan.texture);

    if (buffer == nullptr || texture == nullptr)
        return;

    copyRows(plan,
             [&](const wgpu::TexelCopyBufferLayout& layout, int row, int rows)
             {
                 const auto source = texelCopyOf(
                     *texture, plan.level, plan.x, plan.y + row, plan.slice);

                 auto destination = wgpu::TexelCopyBufferInfo {};
                 destination.layout = layout;
                 destination.buffer = buffer->buffer;

                 const auto size =
                     wgpu::Extent3D {static_cast<std::uint32_t>(plan.width),
                                     static_cast<std::uint32_t>(rows),
                                     1};

                 commands->encoder.CopyTextureToBuffer(&source, &destination, &size);
             });
}

// Device::supportsMipGeneration is false here, so nothing plans one.
void recordMipGeneration(void*, const Texture&) {}
} // namespace eacp::GPU
