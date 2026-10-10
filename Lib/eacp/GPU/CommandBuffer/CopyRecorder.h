#pragma once

#include "CopyPlan.h"

namespace eacp::GPU
{
// Each backend's half of a copy, recorded onto `recording`: the
// id<MTLCommandBuffer> on Metal, the CommandContext* on D3D12 and Vulkan, as
// Frame::nativeRecording and CommandBuffer::nativeRecording hand them out.
// Null records nothing. The plan has already been checked.
void recordCopy(void* recording, const TextureCopyPlan& plan);
void recordCopy(void* recording, const BufferCopyPlan& plan);
void recordCopyToTexture(void* recording, const BufferTextureCopyPlan& plan);
void recordCopyToBuffer(void* recording, const BufferTextureCopyPlan& plan);

// Every level below 0 of every slice filled from the one above it, for a
// texture planMipGeneration has passed.
void recordMipGeneration(void* recording, const Texture& texture);
} // namespace eacp::GPU
